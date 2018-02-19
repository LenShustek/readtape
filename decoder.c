//file: decoder.c
/**********************************************************************

Decode analog 9-track manchester phase encoded magnetic tape data

We are called once for each set of read head voltages on 9 data tracks.

Each track is processed independently, so that head and data skew is
ignored. We look for relative minima and maxima that represent the
downward and upward flux transitions, and use the timing of them to
reconstruct the original data that created the Manchester encoding:
a downward flux transition for 0, an upward flux transition for 1,
and clock transitions as needed in the midpoint between the bits.

We dynamically track the bit timing as it changes, to cover for
variations in tape speed. We can work with any average tape speed,
as long as Faraday's Law produces an analog signal enough above noise.
We are also independent of the number of analog samples per flux
transition, although having 10 or more is good.

If we see a dropout in a track, we wait for data the return. Then
we create "faked" bits to cover for the dropout. We keep a record
of which bits have been faked.

The major routine here is process_sample(). It returns an indication
of whether the current sample represents the end of a block, whose
state is then in struct blkstate_t block.

***********************************************************************
Copyright (C) 2018, Len Shustek

See readtape.c for the merged change log.
***********************************************************************

Ideas for improvement of the decoding algorithms, some of which have
been partially or fully implemented:

- Better idle detection, but distinguished from dropouts.

- Better max/min detection for very low amplitude signals. The challenge
is to tell that from the normal idle noise between blocks.

- When the signal flatlines because of a dropout and we miss data
transitions, continue to fake identical data with the last clock rate
or with the clock of adjacent tracks that seem to be ok.

- Recover blocks where tracks have a different number of data bits,
by calculating relative skew as we go along, and use that to "borrow"
clocks from a good track when data is missing or noisy. This is
related to the suggestion above, but not the same.

- As track data accumulates, check for tracks that are significantly
behind their brethren, and add some fake bits to catch them up. This
will allow otherwise "malformed" block (whose tracks don't all have
the same number of bits) to become well-formed, but with bad parity.
At least some of the data in the block might be good.

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

// Stuff to create a CSV trace file with one track of raw voltage data
// plus all sorts of debugging info. To see the timeline, create a
// line graph in Excel from columns starting with the voltage.
// The start and end of the graph is controlled by code at the bottom.

#define TRACEFILE true	// creating trace file?
#define TRACETRK 4		// for which track
bool trace_on = false;
int trace_lines = 0;
#define TRACE(var,val) {if(TRACEFILE && t->trknum==TRACETRK) trace_##var=trace_##var##_##val;}
float trace_peak;
#define trace_peak_bot 2.00
#define trace_peak_not 2.25
#define trace_peak_top 2.50
float trace_manch;
#define trace_manch_low 2.75
#define trace_manch_high 3.00
float trace_clkwindow;
#define trace_clkwindow_low 3.25
#define trace_clkwindow_high 3.50
float trace_data;
#define trace_data_low 3.75
#define trace_data_high 4.00
float trace_clkedg;
#define trace_clkedg_low 4.25
#define trace_clkedg_high 4.50
float trace_datedg;
#define trace_datedg_low 4.75
#define trace_datedg_high 5.00
float trace_clkdet;
#define trace_clkdet_low 5.25
#define trace_clkdet_high 5.50
float trace_fakedata;
#define trace_fakedata_no 5.75
#define trace_fakedata_yes 6.00

FILE *tracef;
int errcode=0;

// variables for time relative to the start must be double-precision
// delta times for samples close to each other can be single-precision
double timenow=0;
double timestart=0;

int num_trks_idle = NTRKS;
int num_samples = 0;
extern bool terse;

struct parms_t parmsets[MAXPARMSETS] = {  // sets of parameters to try until a block is read correctly
    {

        1.5, 0}  // each is: clk_factor, avg_window
    ,
    {
        1.45, 0	}
    ,
    {
        1.40, 0	}
    ,
    {
        1.50, 10 }
    ,
    {
        1.45, 10 }
    ,
    {
        1.40, 10 }
};

struct trkstate_t trkstate[NTRKS] = { // the current state of all tracks
    0};
uint16_t data[MAXBLOCK+1] = { 		  // the reconstructed data in bits 8..0 for tracks 0..7, then P as the LSB
    0};
uint16_t data_faked[MAXBLOCK+1] = {   // flag for "data was faked" in bits 8..0, for tracks 0..7 then P.
    0};
double data_time[MAXBLOCK+1] = { 	  // the time the last track contributed to this data byte
    0};
struct blkstate_t block;  // the status of the current block

void fatal(char *msg1, char *msg2) {
    log("%s %s\n", msg1, msg2);
    log("errno = %d\n", errno);
    exit(99);
}
void assert(bool t, char *msg1, char *msg2) {
    if (!t) fatal(msg1, msg2);
}

void init_blockstate(void) {	// initialize block state information for multiple reads of a block
    static bool wrote_config = false;
    if (!wrote_config  && !terse) {
        log("--- MOVE THRESHOLD %.3f volts, PEAK THRESHOLD %.3f volts\n", MOVE_THRESHOLD, PEAK_THRESHOLD);
        log("--- default BIT SPACING %.2f usec, CLK FACTOR %.1f, default CLOCK WINDOW %.2f usec\n", //
            BIT_SPACING*1e6, CLK_FACTOR, CLK_WINDOW*1e6);
        log("--- MAX AVG WINDOW %d, BIT FACTOR %.1f, Fake? %d, Multiple tries? %d, Use all parmsets? %d\n", //
           MAX_AVG_WINDOW, BIT_FACTOR, FAKE_BITS, MULTIPLE_TRIES, USE_ALL_PARMSETS);
        wrote_config = true;
    }
    for (int i=0; i<MAXPARMSETS; ++i) {
        block.results[i].blktype = BS_NONE;
        block.results[i].parity_errs = 0;
        block.results[i].faked_bits = 0;
    }
}

void init_trackstate(void) {  // initialize all track and block state information for a new decoding of a block
    num_trks_idle = NTRKS;
    num_samples = 0;
    block.results[block.parmset].blktype = BS_NONE;
    block.results[block.parmset].parity_errs = 0;
    block.results[block.parmset].faked_bits = 0;
    for (int trknum=0; trknum<NTRKS; ++trknum) {
        struct trkstate_t *trk = &trkstate[trknum];
        trk->astate = AS_IDLE;
        trk->trknum = trknum;
        trk->datacount = 0;
        trk->peakcount = 0;
        trk->t_lastbit = 0;
        trk->t_lastpeak = 0;
        trk->idle = true;
        trk->moving = false;
        trk->clknext = false;
        trk->datablock = false;
        trk->t_bitspaceavg = BIT_SPACING;
#if MAX_AVG_WINDOW
        trk->bitndx = 0;
        for (int i=0; i<MAX_AVG_WINDOW; ++i) // initialize moving average bitspacing array
            trk->t_bitspacing[i] = BIT_SPACING;
        trk->t_clkwindow = trk->t_bitspaceavg/2 * parmsets[block.parmset].clk_factor;
#else
        trk->t_clkwindow = CLK_WINDOW;
#endif
#if TRACEFILE
        trace_peak = trace_peak_not;
        trace_manch = trace_manch_low;
        trace_clkwindow = trace_clkwindow_low;
        trace_data = trace_data_low;
        trace_clkdet = trace_clkdet_low;
#endif
    }
}

void show_track_datacounts (char *msg) {
    dlog("%s\n", msg);
    for (int trk=0; trk<NTRKS; ++trk) {
        struct trkstate_t *t = &trkstate[trk];
        dlog("   trk %d has %d data bits, %d peaks, %f avg bit spacing\n",
            trk, t->datacount, t->peakcount, (t->t_lastbit - t->t_firstbit) / t->datacount * 1e6);
    }
}

void end_of_block(void) { // All/most tracks have just become idle. See if we accumulated a data block, a tape mark, or junk

    struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding

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
        result->blktype = BS_TAPEMARK;
        return;
    }

    // to be a valid data block, we remove the postammble and check that all track have the same number of bits

    //  extern byte EBCDIC[];
    //for (int i=0; i<trkstate[0].datacount; ++i)
    //	dlog("%9b %02X %c %d\n", data[i], data[i]>>1, EBCDIC[data[i]>>1], i);

    int  postamble_bits;
    float avg_bit_spacing = 0;
    result->minbits=MAXBLOCK;
    result->maxbits=0;
    for (int trk=0; trk<NTRKS; ++trk) {
        struct trkstate_t *t = &trkstate[trk];
        avg_bit_spacing += (t->t_lastbit - t->t_firstbit) / t->datacount * 1e6;
        //dlog("trk %d firstbit at %.7lf, lastbit at %.7lf, avg spacing %.2f\n", trk, t->t_firstbit, t->t_lastbit, t->avg_bit_spacing);

        //if (trk==1 || trk==3) {
        //	log("trk %d ending bits: ",trk);
        //	for (int i=50; i>=0; --i) log("%d ", (data[t->datacount-i] >> trk) & 1);
        //	log("\n");
        //}

        // weird stuff goes on as the signal dies at the end of a block! So we ignore the last few data bits.
        postamble_bits = 0;
        // todo: if we're removing any faked bits, decrement block.results[block_parmset].faked_bits
        if (t->datacount > IGNORE_POSTAMBLE) t->datacount -= IGNORE_POSTAMBLE;
        while (t->datacount>0 &&  // now remove trailing zeroes (postamble)
            !(data[t->datacount-1] & (0x100 >> trk))) {
            --t->datacount;
            ++postamble_bits;
        }
        dlog("trk %d had %d postamble bits\n", trk, postamble_bits);
        if (t->datacount>0) --t->datacount; // remove the 1 at the start of the postamble;
        if (t->datacount > result->maxbits) result->maxbits = t->datacount;
        if (t->datacount < result->minbits) result->minbits = t->datacount;
    }
    result->avg_bit_spacing = avg_bit_spacing/NTRKS;

    if (result->maxbits == 0) {  // leave result-blktype == BS_NONE
        dlog("   ignoring noise block\n");
    }
    else {
        if (result->minbits != result->maxbits) {  // different number of bits in different tracks
            show_track_datacounts("*** malformed block");
            result->blktype = BS_MALFORMED;
        }
        else {
            result->blktype = BS_BLOCK;
        }
        result->parity_errs = 0;
        for (int i=0; i<result->minbits; ++i) // count parity errors
            if (parity(data[i]) != 1) ++result->parity_errs;
    }
}

void update_clk_average(struct trkstate_t *t, double delta) {
#if MAX_AVG_WINDOW
    int avg_window = parmsets[block.parmset].avg_window;
    if (avg_window > 0) {
        assert(avg_window < MAX_AVG_WINDOW, "avg window in parmset too big", "");
        // update the moving average of bit spacing, and the clock window that depends on it
        float oldspacing = t->t_bitspacing[t->bitndx]; // save value going out of average
        t->t_bitspacing[t->bitndx] = delta; // insert new value
        if (++t->bitndx >= avg_window) t->bitndx = 0; // circularly increment the index
        t->t_bitspaceavg += (delta-oldspacing)/avg_window; // update moving average

        //if (t->trknum == TRACETRK && trace_on) {
        //    dlog("trk %d bit %d at %.7lf new %f old %f avg %f clkwindow %f usec\n", //
        //        t->trknum, t->datacount, timenow, delta*1e6, oldspacing*1e6, t->t_bitspaceavg*1e6, t->t_clkwindow*1e6);
        //}
    }
    else { // not averaging
        t->t_bitspaceavg = BIT_SPACING;
    }
#else
  t->t_bitspaceavg = BIT_SPACING;
#endif
    t->t_clkwindow = t->t_bitspaceavg/2 * parmsets[block.parmset].clk_factor;
}

void addbit (struct trkstate_t *t, byte bit, bool faked, double t_bit) { // we encountered a data bit transition
    float delta;
    if(bit) TRACE(data,high) else TRACE(data,low);
    if (faked) TRACE(fakedata,yes);
    TRACE(datedg,high); 	// data edge
    TRACE(clkwindow,high);	// start the clock window
    if (t->t_lastbit == 0) t->t_lastbit = t_bit - BIT_SPACING; // start of preamble  FIX?
    if (t->datablock) { // collecting data
        t->lastdatabit = bit;
        if (!t->idle && !faked) update_clk_average(t, t_bit - t->t_lastbit);
        t->t_lastbit = t_bit;
        if (t->datacount == 0) t->t_firstbit = t_bit; // record time of first bit in the datablock
        uint16_t mask = 0x100 >> t->trknum;  // update this track's bit in the data array
        data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
        data_faked[t->datacount] = faked ? data_faked[t->datacount] | mask : data_faked[t->datacount] & ~mask;
        if (faked) ++block.results[block.parmset].faked_bits;
        data_time[t->datacount] = t_bit;
        if (t->datacount < MAXBLOCK) ++t->datacount;
    }
}

void check_data_alignment(int clktrk) {
    // this doesn't work; track skew causes some tracks' data bits to legitimately come after other tracks' clocks!
    static int numshown=0;
    int datacount = trkstate[0].datacount;
    for (int trknum=0; trknum<NTRKS; ++trknum) {
        struct trkstate_t *t = &trkstate[trknum];
        if (datacount != t->datacount && numshown<50) {
            dlog("! at clk on trk %d, trk %d has %d databytes, not %d, at %.7lf\n", clktrk, trknum, t->datacount, datacount, timenow);
            ++numshown;
        }
    }
}

void hit_top (struct trkstate_t *t) {  // local maximum: end of a positive flux transition
    t->manchdata = 1;
    TRACE(peak,top);
    ++t->peakcount;
    if (t->datablock) { // inside a data block or the postamble
        if (!t->clknext // if we're expecting a data transition
            || t->t_top - t->t_lastpeak > t->t_clkwindow) { // or we missed a clock
            addbit (t, 1, false, t->t_top);  // then we have new data '1'
            t->clknext = true;
        }
        else { // this was a clock transition
            TRACE(clkedg,high);
            //if(DEBUG) check_data_alignment(t->trknum); //TEMP
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
    t->v_lastpeak = t->v_top;
    t->t_lastpeak = t->t_top;
}

void hit_bot (struct trkstate_t *t) { // local minimum: end of a negative flux transition
    t->manchdata = 0;
    TRACE(peak,bot);
    ++t->peakcount;
    if (t->datablock) { // inside a data block or the postamble
        if (!t->clknext // if we're expecting a data transition
            || t->t_bot - t->t_lastpeak > t->t_clkwindow) { // or we missed a clock
            addbit (t, 0, false, t->t_bot);  // then we have new data '0'
            t->clknext = true;
        }
        else { // this was a clock transition
            TRACE(clkedg,high);
            //if(DEBUG) check_data_alignment(t->trknum); //TEMP
            t->clknext = false;
        }
    }
    else { // inside the preamble
        if (t->peakcount == IGNORE_PREAMBLE) { // ignore all stuff before the nthth peak
            t->clknext = true; // force this to be treated as a data transition
        }
    }
    t->v_lastpeak = t->v_bot;
    t->t_lastpeak = t->t_bot;
}

enum bstate_t process_sample(struct sample_t *sample) {  // process one voltage sample for each track
    float deltaT = sample->time - timenow;  //(incorrect for the first sample)
    timenow = sample->time;

    for (int trknum=0; trknum<NTRKS; ++trknum) {
        struct trkstate_t *t = &trkstate[trknum];
        t->v_now = sample->voltage[trknum];

        //dlog("trk %d state %d voltage %f at %.7lf\n", trknum, t->astate, t->v_now, timenow);
        TRACE(peak,not);
        TRACE(clkedg,low);
        TRACE(datedg,low);
        TRACE(fakedata,no);
        if (timenow - t->t_lastbit > t->t_clkwindow) {
            TRACE(clkwindow,low);
        }
        //if (timenow > 7.8904512 && trknum==1) log("time %.7lf, lastbit %.7lf, delta %lf, clkwindow %lf\n", //
        //    timenow, t->t_lastbit, (timenow-t->t_lastbit)*1e6, t->t_clkwindow*1e6);
        if (t->manchdata) TRACE(manch,high) else TRACE(manch,low);

        if (num_samples == 0)  t->v_lastpeak = t->v_now;

        if (t->astate == AS_IDLE) { // idling, waiting for the start of a flux transition
            t->v_top = t->v_bot = t->v_now;
            t->t_top = t->t_bot = timenow;
            if (t->v_now > t->v_lastpeak + MOVE_THRESHOLD) t->astate = AS_UP;
            if (t->v_now < t->v_lastpeak - MOVE_THRESHOLD) t->astate = AS_DOWN;
            if (t->astate != AS_IDLE){
                dlog ("trk %d not idle, going %s at %.7f, v_now=%f, v_lastpeak=%f, bitspaceavg=%.2f\n", //
                    trknum, t->astate==AS_UP ? "up":"down", timenow, t->v_now, t->v_lastpeak, t->t_bitspaceavg*1e6);
                t->t_lastpeak = timenow;
                --num_trks_idle;
                t->moving = true;
                t->idle = false;
                TRACE(clkdet,high);
                // if we're in a datablock, add extra data bits the same as the last bit to cover the gap
                if (FAKE_BITS && t->datablock && t->datacount>1) {
                    //  Transitions have returned within a data block after a gap.
                    //  Add extra data bits that are same as the last bit before the gap started, in an
                    //  attempt to keep all the tracks in sync. .
                    //int numbits = (timenow - t->t_lastbit) / t->t_bitspaceavg;
                    //  That computations of the number of bits to add based on the time between now and the last bit
                    //  and the average bit spacing doesn't seem to work, because the clock may have drifted.
                    //  Instead: add enough bits to give this track the same number as the minimum of
                    //  any track which is still getting data. This only occassionally works. Better ideas??
                    int numbits = INT_MAX;
                    for (int i=0; i<NTRKS; ++i) {
                        if (i!=trknum && !trkstate[i].idle && trkstate[i].datacount < numbits) numbits = trkstate[i].datacount;
                    }
                    if (numbits != INT_MAX & numbits > t->datacount) {
                        numbits -= t->datacount;
                        dlog("trk %d adding %d fake bits to %d bits at %.7lf, lastbit at %.7lf, bitspaceavg=%.2f\n", //
                            trknum, numbits, t->datacount, timenow, t->t_lastbit, t->t_bitspaceavg*1e6);
                        show_track_datacounts("*** before adding bits");
                        while (numbits--) addbit(t, t->lastdatabit, true, timenow);
                        t->t_lastbit = 0; // don't let the bitspacing averaging algorithm work on these bits
                        if (t->lastdatabit==0 && t->astate==AS_DOWN || t->lastdatabit==1 && t->astate==AS_UP) {
                            // the first new peak will be a data bit, when it comes
                            t->clknext = false;
                        }
                        else { // the first new peak will be a clock bit, when it comes
                            t->clknext = true;
                            TRACE(clkwindow,low);
                        }
                    }
                }
                goto new_maxmin;
            }
        } // AS_IDLE

        else { // going up or down: check for having waited too long for a peak
            if (t->t_lastpeak != 0 && timenow - t->t_lastpeak > /*BIT_SPACING*2*/ t->t_bitspaceavg * BIT_FACTOR ) {
                t->astate = AS_IDLE;
                t->v_lastpeak = t->v_now;
                t->idle = true;
                TRACE(clkdet,low);
                dlog("trk %d became idle at %.7lf, last peak at %.7lf, bitspaceavg %.2f usec, datacount %d\n", //
                    trknum, timenow, t->t_lastpeak, t->t_bitspaceavg*1e6, t->datacount);
                if (++num_trks_idle >= IDLE_TRK_LIMIT) {
                    end_of_block();
                }
            }
        }

new_maxmin:

        if (t->astate == AS_UP) {  // we are moving up towards a maximum
            if (t->v_now > t->v_lastpeak + MOVE_THRESHOLD) // if we've really started moving
                t->moving = true;
            if (t->moving) {
                // There are four cases to consider:
                // 1. move up a lot: just record a new maximum
                // 2. move up a little: assume we're rounding the top; record the new top but with interpolated time
                // 3. move down a lot: record the previous top as a peak, and change mode to DOWN
                // 4. move down a little: if the previous top's time was interpolated, adjust it back to the previous peak
                //      otherwise interpolate the time with the new point.  Then record the peak and change mode to DOWN.
                // We currently assume monotonicity of the samples! We could elborate the algorithms to fix that.
                float deltaV = t->v_now - t->v_top;
                if (deltaV > PEAK_THRESHOLD) { // still moving up by a lot
                    t->v_top = t->v_now; // just record the new maximum
                   t->t_top = timenow;
                }
                else if (deltaV >= 0) {  // moved up by a little, or stayed the same
                    t->v_top = t->v_now;
                    t->t_top = timenow - deltaT/2; // we could do some interpolation based on how "little" is compared to PEAK_THRESHOLD
                    //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated top going up  at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_top);
                }
                else { // moving down
                    if (deltaV >= -PEAK_THRESHOLD) { // move down, but only by a little
                        //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("  timenow=%.9f, t_top=%.9f\n", timenow, t->t_top);
                        if (t->t_top < timenow - deltaT - EPSILON_T) t->t_top = timenow - deltaT; // erase previous interpolation
                        else t->t_top = timenow - deltaT/2;
                        //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated top going down at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_top);
                    } // else moving down by a lot: keep previous top time
                    hit_top(t);  // record a peak
                    t->astate = AS_DOWN; // start moving down
                    t->moving = false; // but haven't started a big move yet
                    t->v_bot = t->v_now; // start tracking the bottom on the way down
                }
            }
         }

         if (t->astate == AS_DOWN) {  // we are moving down towards a minimum
             if (t->v_now < t->v_lastpeak - MOVE_THRESHOLD) // if we've really started moving
                 t->moving = true;
            if (t->moving) {
                // The above four cases apply, mutatis mutandis, here as well.
               float deltaV = t->v_now - t->v_bot;
                if (deltaV < -PEAK_THRESHOLD) { // still moving down by a lot
                    t->v_bot = t->v_now; // just record the new miminum
                    t->t_bot = timenow;
                }
                else if (deltaV <= 0) {  // moved down by a little, or stayed the same
                    t->v_bot = t->v_now;
                    t->t_bot = timenow - deltaT/2; // we could do some interpolation based on how "little" is compared to PEAK_THRESHOLD
                    //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated bot going down at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_bot);
                }
                else { // moving up
                    if (deltaV <= PEAK_THRESHOLD) { // move up, but only by a little
                        if (t->t_bot < timenow - deltaT - EPSILON_T) t->t_bot = timenow - deltaT; // erase previous interpolation
                        else t->t_bot = timenow - deltaT/2;
                        //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated bot going up   at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_bot);
                    } // else moving up by a lot: keep previous bottom time
                    hit_bot(t);  // record a peak
                    t->astate = AS_UP; // start moving up
                    t->moving = false; // but haven't started a big move yet
                    t->v_top = t->v_now; // start tracking the top on the way up
                }
            }
         }

    } // for tracks

#if TRACEFILE
    extern char *basefilename;
    if (!tracef) {
        char filename[MAXPATH];
        sprintf(filename, "%s\\trace.csv", basefilename);
        assert (tracef = fopen(filename, "w"), "can't open trace file:", filename);
        fprintf(tracef, "TRK%d time, voltage, peak, manch, clkwind, data, clkedg, datedg, clkdet, fakedata, bitspaceavg, bitspacing0\n", TRACETRK);
    }
    // Put special tests here for turning the trace on, depending on what anomaly we're looking at...
    if(timenow > 4.4826573) trace_on = true;
    //if (trkstate[0].datablock) trace_on = true;
    //if (num_samples == 8500) trace_on = true;
    //if (trkstate[0].datacount == 275) trace_on = true;
    //if (trkstate[3].datacount > trkstate[0].datacount+1) trace_on = true;
    if (trace_on) {
        if (trace_lines < 250) { // limit on how much trace data to collect
            if (timestart==0) timestart=timenow;
            fprintf(tracef, "%.8lf, %f, ", timenow, sample->voltage[TRACETRK]);
            fprintf(tracef, "%f, %f, %f, %f, %f, %f, %f, %f, %.2f, %.2f\n", //
                trace_peak, trace_manch, trace_clkwindow, trace_data, trace_clkedg, trace_datedg, trace_clkdet, trace_fakedata,
                trkstate[TRACETRK].t_bitspaceavg*1e6, trkstate[0].t_bitspaceavg*1e6);
            ++trace_lines;
        }
    }
#endif

    ++num_samples;
    return block.results[block.parmset].blktype;  // what block type we found, if any
}

//*

