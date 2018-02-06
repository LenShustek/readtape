//file: decoder.c
/**********************************************************************

Decode analog 9-track manchester phase encoded magnetic tape data

We are called once for each set of read head voltages on 9 data tracks.

Each track is processed independently, so that head and data skew is
ignored. We look for relative minima and maxima that represent the
downward and upward flux transitions, and use the timing of them to
reconstruct the original data that created the manchester encoding:
a downward flux transition for 0, an upward flux transition for 1,
and clock transitions as needed in the midpoint between the bits.

We dynamically track the bit timing as it changes, to cover for
variations in tape speed.

When a tape block has been passed, we call one of three exit routines:

got_tapemark() when we found the bizarre configuration of a tapemark
got_datablock() when we found a data block, maybe with parity errors
got_crap() when we found a block whose tracks aren't the same length

***********************************************************************
Copyright (C) 2018, Len Shustek

---CHANGE LOG

20 Jan 2018, L. Shustek, Started first version.
05 Feb 2018, L. Shustek, First github posting, since it kinda works.

***********************************************************************
TODO:

- Better idle detection, but distinguished from dropouts

- Better max/min detection for very low amplitude signals. The challenge
is to tell that from the normal idle noise between blocks.

- When the signal flatlines because of a dropout and we miss data
transitions, continue to fake identical data with the last clock rate

- Recover blocks where tracks have a different number of data bits,
by calculating relative skew as we go along, and use that to "borrow"
clocks from a good track when data is missing or noisy. This is
related to the suggestion above, but not the same.

***********************************************************************
The MIT License (MIT): Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*************************************************************************/

#include <decoder.h>

// lots of of parameters that control the decoding algorithm

#define REST_THRESHOLD	0.15	// volts of deviation that means something's happening
#define PEAK_THRESHOLD	0.03 	// volts per volt that define "same peak"   (was .01 to work when not per volt)
#define IDLE_TRIGGER	50e-6 	// in seconds, how long a lack of signal indicates a interblock gap
#define BIT_SPACING		12.5e-6	// in seconds, the default bit spacing (1600 BPI x 50 IPS)
#define CLK_WINDOW	 	8.5e-6	// in seconds, the default max wait for a clock edge
#define CLK_FACTOR		1.4		// how much of a period to wait for the clock transition.
#define AVG_WINDOW		2		// how many bit times to include in the clock timing moving average (0 means use defaults)
#define IDLE_THRESHOLD	9		// how many tracks must be idle to consider it an end-of-block

#define IGNORE_PREAMBLE		5		// how many preamble bits to ignore
#define IGNORE_POSTAMBLE	5		// how many postable bits to ignore
#define MIN_PREAMBLE		70		// minimum number of peaks (half that number of bits) for a preamble

#define DEBUG 0

#if DEBUG
#define dlog(...) log(__VA_ARGS__) // debugging log
#else
#define dlog(...)
#endif

// stuff to create a CSV trace file with one track of data and all sort of debugging info
// create a line graph in Excel with columns starting with the voltage

#define TRACEFILE true	// creating trace file?
#define TRACETRK 5		// for which track
bool trace_on = false;
int trace_lines = 0;
#define TRACE(var,val) {if(TRACEFILE && t->trknum==TRACETRK) trace_##var=trace_##var##_##val;}
float trace_peak;
#define trace_peak_bot 2
#define trace_peak_not 2.25
#define trace_peak_top 2.5
float trace_manch;
#define trace_manch_low 2.75
#define trace_manch_high 3.25
float trace_clkwindow;
#define trace_clkwindow_low 3.5
#define trace_clkwindow_high 4
float trace_data;
#define trace_data_low 4.25
#define trace_data_high 4.75
float trace_clkedg;
#define trace_clkedg_low 5
#define trace_clkedg_high 5.25
float trace_datedg;
#define trace_datedg_low 5.5
#define trace_datedg_high 5.75

FILE *tracef;
int errcode=0;

double timenow;  	// all times should be double precision!
double timestart=0;
int num_trks_idle = NTRKS;
enum astate_t { // analog track state
	AS_IDLE, AS_UP, AS_DOWN};

struct trkstate_t {	// track-by-track decoding state
	int trknum;				// which track number 0..8, where 8=P
	enum astate_t astate;	// current state: AS_xxx
	float v_now;  			// current voltage
	float v_top; 			// last top voltage
	double t_top; 			// time of last top
	float v_bot;			// last bottom voltage
	double t_bot;			// time of last bottom
	double t_lastpeak;		// time of last top or bottom
	double t_lastbit;		// time of last data bit transition
	double t_firstbit;		// time of first data bit transition in the data block
	double t_clkwindow; 	// how late a clock transition can be, before we consider it data
	float avg_bit_spacing;	// how far aparts bits are, on average, recently
	float v_resting;		// the resting voltage for this track
	double t_resting;		// when we recorded the resting voltage
#if AVG_WINDOW
	float t_bitspacing[AVG_WINDOW]; // last n bit time spacing
	float t_bitspaceavg;	// average of laast n bit time spacing
	int	bitndx;				// index into t_bitspacing of next spot to use
#endif
	int datacount;			// how many data bits we've seen
	int peakcount;			// how many peaks (flux reversals) we've seen
	bool manchdata;			// the reconstructed manchester encoding
	bool moving;			// are we moving up or down through a transition?
	bool clknext;			// do we expect a clock next?
	bool datablock;			// are we collecting data?
}
trkstate[NTRKS] = {
	0};

uint16_t data[MAXBLOCK+1] = { // the reconstructed data in bits 8..0: tracks 0..7, then P as the LSB
	0};

void fatal(char *msg1, char *msg2) {
	log("%s %s\n", msg1, msg2);
	exit(99);
}
void assert(bool t, char *msg1, char *msg2) {
	if (!t) fatal(msg1, msg2);
}

void init_trackstate(void) {  // initialize all track state information for a new block
	static bool wrote_config = false;
	if (!wrote_config) {
		log("--- rest threshold %.3f volts, peak threshold %.3f volts\n", REST_THRESHOLD, PEAK_THRESHOLD);
		log("--- idle trigger %.2f usec, default bit spacing %.2f usec, default clock window %.2f usec\n", //
			IDLE_TRIGGER*1e6, BIT_SPACING*1e6, CLK_WINDOW*1e6);
		log("--- clock window factor %.2f, average window is %d bits wide\n", CLK_FACTOR, AVG_WINDOW);
		wrote_config = true;
	}
	num_trks_idle = NTRKS;
	for (int trknum=0; trknum<NTRKS; ++trknum) {
		struct trkstate_t *trk = &trkstate[trknum];
		trk->astate = AS_IDLE;
		trk->trknum = trknum;
		trk->datacount = 0;
		trk->peakcount = 0;
		trk->t_lastbit = 0;
		trk->moving = false;
		trk->clknext = false;
		trk->datablock = false;
		trk->v_resting = 0;
		trk->t_resting = 0;
#if AVG_WINDOW
		trk->bitndx = 0;
		trk->t_bitspaceavg = BIT_SPACING;
		for (int i=0; i<AVG_WINDOW; ++i) // initialize moving average bitspacing array
			trk->t_bitspacing[i] = BIT_SPACING;
		trk->t_clkwindow = trk->t_bitspaceavg/2 * CLK_FACTOR;
#else
		trk->t_clkwindow = CLK_WINDOW;
#endif
#if TRACEFILE
		trace_peak = trace_peak_not;
		trace_manch = trace_manch_low;
		trace_clkwindow = trace_clkwindow_low;
		trace_data = trace_data_low;
#endif
	}
}

void end_of_block(void) { // All/most tracks have just become idle. See if we accumulated a data block, a tape mark, or junk

	// a tape mark is bizarre:
	// 	-- 80 or more flux reversals (but no data bits) on tracks 0, 2, 5, 6, 7, and P
	//  -- no flux reversals (DC erased) on tracks 1, 3, and 4
	// We actually allow a couple of data bits because of weirdness when the flux transitions.stop
	if (trkstate[0].datacount == 0 && trkstate[0].peakcount > 75 &&
		    trkstate[0].datacount <= 2 && trkstate[0].peakcount > 75 &&
		    trkstate[2].datacount <= 2 && trkstate[2].peakcount > 75 &&
		    trkstate[5].datacount <= 2 && trkstate[5].peakcount > 75 &&
		    trkstate[7].datacount <= 2 && trkstate[7].peakcount > 75 &&
		    trkstate[8].datacount <= 2 && trkstate[8].peakcount > 75 &&
		    trkstate[1].peakcount <= 2 &&
		    trkstate[3].peakcount <= 2 &&
		    trkstate[4].peakcount <= 2
		    ) { // got a tape mark
		got_tapemark();
		goto cleanup;
	}

	// to be a valid data block, we remove the postammble and check that all track have the same number of bits

	//  extern byte EBCDIC[];
	//for (int i=0; i<trkstate[0].datacount; ++i)
	//	dlog("%9b %02X %c %d\n", data[i], data[i]>>1, EBCDIC[data[i]>>1], i);

	int numbits, postamble_bits;
	bool crap = false;
	float avg_bit_spacing = 0;
	for (int trk=0; trk<NTRKS; ++trk) {
		struct trkstate_t *t = &trkstate[trk];
		t->avg_bit_spacing = (t->t_lastbit - t->t_firstbit) / t->datacount * 1e6;
		avg_bit_spacing += t->avg_bit_spacing;

		//if (trk==1 || trk==3) {
		//	log("trk %d ending bits: ",trk);
		//	for (int i=50; i>=0; --i) log("%d ", (data[t->datacount-i] >> trk) & 1);
		//	log("\n");
		//}

		// weird stuff goes on as the signal dies at the end! So we ignore the last few data bits.
		postamble_bits = 0;
		if (t->datacount > IGNORE_POSTAMBLE) t->datacount -= IGNORE_POSTAMBLE;
		while (t->datacount>0 &&  // now remove trailing zeroes (postamble)
			!(data[t->datacount-1] & (0x100 >> trk))) {
			--t->datacount;
			++postamble_bits;
		}
		dlog("trk %d had %d postamble bits\n", trk, postamble_bits);
		if (t->datacount>0) --t->datacount; // remove the 1 at the start of the postamble;
		if (trk == 0) numbits = t->datacount; // make sure all tracks have the same number of bits
		else if (t->datacount != numbits) crap = true;
	}
	if (crap) {
		for (int trk=0; trk<NTRKS; ++trk)
			log("   trk %d has %d data bits, %d peaks, %f avg bit spacing\n",
				trk, trkstate[trk].datacount, trkstate[trk].peakcount, trkstate[trk].avg_bit_spacing);
		got_crap(numbits);
		goto cleanup;
	}

	// we have a set of equal-length data: must be a data block
	got_datablock(data, numbits, avg_bit_spacing/NTRKS);

cleanup:
	init_trackstate();  // initialize for next block
}

void update_clk_average(struct trkstate_t *t, double delta) {
#if AVG_WINDOW
	// update the moving average of bit spacing, and the clock window that depends on it
	float oldspacing = t->t_bitspacing[t->bitndx]; // save value going out of average
	t->t_bitspacing[t->bitndx] = delta; // insert new value
	if (++t->bitndx >= AVG_WINDOW) t->bitndx = 0; // circularly increment the index
	t->t_bitspaceavg += (delta-oldspacing)/AVG_WINDOW; // update moving average
	t->t_clkwindow = t->t_bitspaceavg/2 * CLK_FACTOR;
	//if (t->trknum == TRACETRK && trace_on) {
	//	dlog("trk %d at %.7lf new %f old %f avg %f clkwindow %f usec\n", //
	//		 t->trknum, timenow, delta*1e6, oldspacing*1e6, t->t_bitspaceavg*1e6, t->t_clkwindow*1e6);
	//}
#else
	t->t_clkwindow = CLK_WINDOW;
#endif

}

void addbit (struct trkstate_t *t, int bit, double t_bit) { // we encountered a data bit transition
	float delta;
	if(bit) TRACE(data,high) else TRACE(data,low);
	TRACE(datedg,high); 	// data edge
	TRACE(clkwindow,high);	// start the clock window
	if (t->t_lastbit == 0) t->t_lastbit = t_bit - 12.5e-6; // start of preamble
	if (t->datablock) { // collecting data
		update_clk_average(t, t_bit - t->t_lastbit);
		t->t_lastbit = t_bit;
		if (t->datacount == 0) t->t_firstbit = t_bit; // record time of first bit in the datablock
		uint16_t mask = 0x100 >> t->trknum;  // update this track's bit in the data array
		data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
		if (t->datacount < MAXBLOCK) ++t->datacount;
	}
}

void hit_top (struct trkstate_t *t) {  // local maximum: end of a positive flux transition
	t->manchdata = 1;
	TRACE(peak,top);
	++t->peakcount;
	if (t->datablock) { // inside a data block or the postamble
		if (!t->clknext // if we're expecting a data transition
			|| t->t_top - t->t_lastpeak > t->t_clkwindow) { // or we missed a clock
			addbit (t, 1, t->t_top);  // then we have new data '1'
			t->clknext = true;
		}
		else { // this was a clock transition
			TRACE(clkedg,high);
			t->clknext = false;
		}
	}
	else { // inside the preamble
		if (t->peakcount == IGNORE_PREAMBLE) { // ignore all stuff before the 5th peak
			t->clknext = false; // force this to be treated as a clock transition
		}
		else if (t->peakcount > MIN_PREAMBLE	// if we've seen at least 35 zeroes
			&& t->t_top - t->t_lastpeak > t->t_clkwindow) { // and we missed a clock
			t->datablock = true;	// then this 1 means data is starting (end of preamble)
			dlog("trk %d start data at %.7lf, clock window %lf usec\n", t->trknum, timenow, t->t_clkwindow*1e6);
		}
	}
	t->t_lastpeak = t->t_top;
}

void hit_bot (struct trkstate_t *t) { // local minimum: end of a negative flux transition
	t->manchdata = 0;
	TRACE(peak,bot);
	++t->peakcount;
	if (t->datablock) { // inside a data block or the postamble
		if (!t->clknext // if we're expecting a data transition
			|| t->t_bot - t->t_lastpeak > t->t_clkwindow) { // or we missed a clock
			addbit (t, 0, t->t_bot);  // then we have new data '0'
			t->clknext = true;
		}
		else { // this was a clock transition
			TRACE(clkedg,high);
			t->clknext = false;
		}
	}
	else { // inside the preamble
		if (t->peakcount == IGNORE_PREAMBLE) { // ignore all stuff before the nthth peak
			t->clknext = true; // force this to be treated as a data transition
		}
	}
	t->t_lastpeak = t->t_bot;
}

void process_sample(struct sample_t *sample) {
	timenow = sample->time;

	for (int trknum=0; trknum<NTRKS; ++trknum) {
		struct trkstate_t *t = &trkstate[trknum];
		t->v_now = sample->voltage[trknum];
		if (t->t_resting == 0) {  // this is the first voltage sample we're seeing
			t->v_resting = t->v_now;
			t->t_resting = timenow;
		}
		//dlog("trk %d state %d voltage %f at %.7lf\n", trknum, t->astate, t->v_now, timenow);
		TRACE(peak,not);
		TRACE(clkedg,low);
		TRACE(datedg,low);
		if (timenow - t->t_lastbit > t->t_clkwindow) {
			TRACE(clkwindow,low);
		}
		//if (timenow > 7.8904512 && trknum==1) log("time %.7lf, lastbit %.7lf, delta %lf, clkwindow %lf\n", //
		//    timenow, t->t_lastbit, (timenow-t->t_lastbit)*1e6, t->t_clkwindow*1e6);
		if (t->manchdata) TRACE(manch,high) else TRACE(manch,low);

		if (t->astate == AS_IDLE) { // waiting for flux transitions
			t->v_top = t->v_bot = t->v_now;
			t->t_top = t->t_bot = timenow;
			t->clknext = false;
			if (t->v_now > t->v_resting + REST_THRESHOLD) {
				dlog ("trk %d not idle, going up at %.7f, v_rest=%f\n", trknum, timenow, t->v_resting);
				t->astate = AS_UP;
				--num_trks_idle;
				t->moving = true;
				t->v_resting = t->v_now;
				t->t_resting = timenow;
				goto new_top;
			}
			else if (t->v_now < t->v_resting - REST_THRESHOLD) {
				dlog ("trk %d not idle, going down at %.7f, v_rest=%f\n", trknum, timenow, t->v_resting);
				t->astate = AS_DOWN;
				--num_trks_idle;
				t->moving = true;
				t->v_resting = t->v_now;
				t->t_resting = timenow;
				goto new_bot;
			}
		}
		else { // either going up or down: check for lingering too long at a resting voltage
			if (t->v_now < t->v_resting + REST_THRESHOLD && t->v_now > t->v_resting - REST_THRESHOLD) {
				//static bool diverged = false;
				//if (!diverged && t->trknum==5 && abs(t->datacount-trkstate[0].datacount)>1) {
				//	dlog("trk %d diverged at %.7lf\n", t->trknum, timenow);
				//	diverged = true;
				//}
				if (timenow - t->t_resting > IDLE_TRIGGER) {
					t->astate = AS_IDLE;
					t->datablock = false;
					dlog("trk %d became idle at %.7lf, v_rest=%f, datacount %d\n", trknum, timenow, t->v_resting, t->datacount);
					if (++num_trks_idle >= IDLE_THRESHOLD) {
						end_of_block();
					}
				}
			}
			else { // we've moved: change the resting voltage
				t->v_resting = t->v_now;
				t->t_resting = timenow;
			}
		}

		if (t->astate == AS_UP) {
			if (t->moving && t->v_now > t->v_top) { // still going up
new_top:
				//dlog("trk %d new top %.3f\n", trknum, t->v_now);
				t->v_top = t->v_now;
				t->t_top = timenow;
			}
			else {  // just passed through the top
				if (t->moving) hit_top(t);
				t->moving = false;
				if (t->v_now < t->v_top-PEAK_THRESHOLD*t->v_top) { // have really started down
					//dlog("trk %d top %.3f at %.7lf delta %.7lf\n", trknum, t->v_top, t->t_top, t->t_top - t->t_bot);
					t->astate = AS_DOWN;
					t->moving = true;
					t->v_bot = t->v_now; // start tracking the bottom on the way down
				}
				// else: jittering around the peak
			}
		}

		else if (t->astate == AS_DOWN) {
			if (t->moving && t->v_now < t->v_bot) { // still going down
new_bot:
				//dlog("trk %d new bot %.3f\n", trknum, t->v_now);
				t->v_bot = t->v_now;
				t->t_bot = timenow;
			}
			else {  // just passed through the bottom
				if (t->moving) hit_bot(t);
				t->moving = false;
				if (t->v_now > t->v_bot+PEAK_THRESHOLD*t->v_top) {
					//dlog("trk %d bot %.3f at %.7lf delta %.7lf\n", trknum, t->v_bot, t->t_bot, t->t_bot - t->t_top);
					t->astate = AS_UP;
					t->moving = true;
					t->v_top = t->v_now; // start tracking the top on the way up
				}
				// else: jittering around the bottom
			}
		}

		else if (t->astate != AS_IDLE) fatal ("bad astate", "");
	} // for tracks

#if TRACEFILE
	extern char *basefilename;
	if (!tracef) {
		char filename[MAXPATH];
		sprintf(filename, "%s\\trace.csv", basefilename);
		assert (tracef = fopen(filename, "w"), "can't open trace file:", filename);
		fprintf(tracef, "TRK%d time, delta, voltage, peak, manch, clkwind, data, clkedg, datedg, v_rest, t_rest\n", TRACETRK);
	}
	// put special tests for turning the trace on here, depending on what anomaly we're looking at...
	if(timenow > 0.9426125 - 130e-6) trace_on = true;
	//if (trkstate[0].datablock) trace_on = true;
	if (trace_on) {
		if (trace_lines < 2000) {
			if (timestart==0) timestart=timenow;
			fprintf(tracef, "%.8lf, %8.2lf, %f, ", timenow, (timenow-timestart)*1e6, sample->voltage[TRACETRK]);
			fprintf(tracef, "%f, %f, %f, %f, %f, %f, %f, %f\n", //
				trace_peak, trace_manch, trace_clkwindow, trace_data, trace_clkedg, trace_datedg, //
				trkstate[TRACETRK].v_resting, trkstate[TRACETRK].t_resting);
			++trace_lines;
		}
		else trace_on = false;
	}
#endif

}

//*
