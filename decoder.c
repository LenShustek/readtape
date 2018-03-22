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

We compute the natural peak-to-peak amplitude of the signal from
each head by sampling a few dozen bits during the preamble.
Then during the data part of the block, we increase the simulated
gain when the peak-to-peak amplitude decreases. This AGC (automatic
gain control) works well for partial dropouts during a block.

If we see a total dropout in a track, we wait for data the return.
Then we create "faked" bits to cover for the dropout. We keep a
record of which bits have been faked.

The major routine here is process_sample(). It returns an indication
of whether the current sample represents the end of a block, whose
state is then in struct blkstate_t block.

----> See readtape.c for the merged change log.

***********************************************************************
Copyright (C) 2018, Len Shustek
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

#include "decoder.h"

// sets of parameters to try until a block is read correctly, or with mininal errors
// int clk_window;         // how many bit times to window average for clock rate; 0 means use exponential averaging
// float clk_alpha;			// weighting for current data in the clock rate exponential weighted average; 0 means use constant
// float move_threshold;	// how many volts of deviation means that we have a real signal
// float zero_band;        // how many volts close to zero should be considered zero
//										(the AGC algorithm might reduce the last two dynamically as the signal degrades)
// float clk_factor;	      // PE: how much of a half-bit period to wait for a clock transition
// float pulse_adj_amt;    // PE: how much of the previous pulse's deviation to adjust this pulse by, 0 to 1

struct parms_t parmsets_PE[MAXPARMSETS] = {  //*** parmsets for 1600 BPI PE ***
   // clkwin  clkalpha   move    zero  clkfact  pulseadj
   {1,   0,    0.2,    0.10,   0.05,   1.50,   0.4,  "PRM" },
   {1,   3,    0.0,    0.10,   0.01,   1.40,   0.0,  "PRM" }, // works on block 55, but not with pulseadj=0.2
   {1,   3,    0.0,    0.10,   0.01,   1.40,   0.2,  "PRM" },
   {1,   5,    0.0,    0.10,   0.01,   1.40,   0.0,  "PRM" },
   {1,   5,    0.0,    0.10,   0.01,   1.50,   0.2,  "PRM" },
   {1,   5,    0.0,    0.10,   0.01,   1.40,   0.4,  "PRM" },
   {1,   3,    0.0,    0.05,   0.01,   1.40,   0.2,  "PRM" },
   {0 } };

struct parms_t parmsets_NRZI[MAXPARMSETS] = { //*** parmsets for 800 BPI NRZI ***
   // clkwin  clkalpha  move    zero
   {1,  0,     0.4,    0.10,   0.50,   0, 0, "PRM" },
   {1,  0,     0.3,    0.10,   0.50,   0, 0, "PRM" },
   {1,  0,     0.2,    0.10,   0.50,   0, 0, "PRM" },
   {0 } };

struct parms_t parmsets_GCR[MAXPARMSETS] = { //*** parmsets for 6250 BPI GCR ***
   // clkwin  clkalpha  move    zero
   {1,  0,     0.4,    0.10,   0.50,  0, 0, "PRM" },
   {0 } };

struct parms_t *parmsetsptr;  // pointer to parmsets array for this decoding type (PE, NRZI, GCR)


// Stuff to create a CSV trace file with one track of raw voltage data
// plus all sorts of debugging info. To see the timeline, create a
// line graph in Excel from columns starting with the voltage.
// The on-switch and the track number is in decoder.h.
// The start and end of the graph is controlled by code at the bottom of this file.

bool trace_on = false;
bool trace_done = false;
int trace_lines = 0;
#define TRACE(var,val) {if(DEBUG && TRACEFILE && t->trknum==TRACETRK) trace_##var=trace_##var##_##val;}
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
float trace_midbit;
#define trace_midbit_low 6.25
#define trace_midbit_high 6.00


// Variables for time relative to the start must be double-precision.
// Delta times for samples close to each other can be single-precision,
// although the compiler will complain about loss of precision.

double timenow=0;
int num_trks_idle = NTRKS;
int num_samples = 0;
FILE *tracef;
int errcode=0;
double interblock_expiration = 0; // interblock gap expiration time

struct nrzi_t nrzi = { 0 }; // NRZI decoding status
float nrzi_starting_bitrate = NRZI_CLK_DEFAULT;
float bpi = 1600;

struct trkstate_t trkstate[NTRKS] = { // the current state of all tracks
   0 };
uint16_t data[MAXBLOCK+1] = { 		  // the reconstructed data in bits 8..0 for tracks 0..7, then P as the LSB
   0 };
uint16_t data_faked[MAXBLOCK+1] = {   // flag for "data was faked" in bits 8..0 for tracks 0..7, then P as the LSB
   0 };
double data_time[MAXBLOCK+1] = { 	  // the time the last track contributed to this data byte
   0 };
struct blkstate_t block;  // the status of the current data block as decoded with the various sets of parameters

/*****************************************************************************************************************************
   Routines for all encoding types
******************************************************************************************************************************/
void init_blockstate(void) {	// initialize block state information for multiple reads of a block
   static bool wrote_config = false;
#if 0 // needs elaboration for new parms
   if (!wrote_config  && verbose) {
      rlog("settings: peak threshold %.3f volts, default bit spacing %.2f usec\n", PEAK_THRESHOLD, BIT_SPACING*1e6);
      rlog("settings: Fake? %d, Multiple tries? %d, Use all parmsets? %d\n", FAKE_BITS, multiple_tries, USE_ALL_PARMSETS);
      wrote_config = true; }
#endif
   for (int i=0; i<MAXPARMSETS; ++i) {
      assert(parmsetsptr[i].clk_factor == 0 || strcmp(parmsetsptr[i].id, "PRM") == 0, "bad parm block initialization");
      //block.results[i].blktype = BS_NONE;
      //block.results[i].parity_errs = 0;
      //block.results[i].faked_bits = 0;
      //block.results[i].alltrk_max_agc_gain = 1.0;
   } }

void init_trackstate(void) {  // initialize all track and block state information for a new decoding of a block
   num_trks_idle = NTRKS;
   num_samples = 0;
   interblock_expiration = 0;
   memset(&block.results[block.parmset], 0, sizeof(struct results_t));
   block.results[block.parmset].blktype = BS_NONE;
   //block.results[block.parmset].parity_errs = 0;
   //block.results[block.parmset].faked_bits = 0;
   block.results[block.parmset].alltrk_max_agc_gain = 1.0;
   memset(trkstate, 0, sizeof(trkstate));  // only need to initialize non-zeros below
   for (int trknum=0; trknum<NTRKS; ++trknum) {
      struct trkstate_t *trk = &trkstate[trknum];
      trk->astate = AS_IDLE;
      trk->trknum = trknum;
      trk->idle = true;
      trk->clkavg.t_bitspaceavg = mode == PE ? BIT_SPACING : 1 / nrzi_starting_bitrate;
      trk->agc_gain = 1.0;
      trk->last_move_threshold = parmsetsptr[block.parmset].move_threshold;
      trk->max_agc_gain = 1.0;
      for (int i=0; i<CLKRATE_WINDOW; ++i) // initialize moving average bitspacing array
         trk->clkavg.t_bitspacing[i] = BIT_SPACING;
      trk->t_clkwindow = trk->clkavg.t_bitspaceavg/2 * parmsetsptr[block.parmset].clk_factor; }
   memset(&nrzi, 0, sizeof(nrzi));  // only need to initialize non-zeros below
   nrzi.clkavg.t_bitspaceavg = 1 / nrzi_starting_bitrate;
#if DEBUG && TRACEFILE
   trace_peak = trace_peak_not;
   trace_manch = trace_manch_low;
   trace_clkwindow = trace_clkwindow_low;
   trace_data = trace_data_low;
   trace_clkdet = trace_clkdet_low;
   trace_midbit = trace_midbit_low;
#endif
}

void show_track_datacounts (char *msg) {
   dlog("%s\n", msg);
   for (int trk=0; trk<NTRKS; ++trk) {
      struct trkstate_t *t = &trkstate[trk];
      dlog("   trk %d has %d data bits, %d peaks, %f avg bit spacing\n",
           trk, t->datacount, t->peakcount, (t->t_lastbit - t->t_firstbit) / t->datacount * 1e6); } }

void check_data_alignment(int clktrk) {
   // this doesn't work for PE; track skew causes some tracks' data bits to legitimately come after other tracks' clocks!
   static int numshown = 0;
   int datacount = trkstate[0].datacount;
   for (int trknum = 0; trknum<NTRKS; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      if (datacount != t->datacount && numshown<50) {
         dlog("! at clk on trk %d, trk %d has %d databytes, not %d, at %.7lf\n", clktrk, trknum, t->datacount, datacount, timenow);
         ++numshown; } } }

void adjust_agc(struct trkstate_t *t) { // update the automatic gain control level
   float move_threshold = parmsetsptr[block.parmset].move_threshold; // the default move threshold for this set of parms
   assert(!AGC_AVG || !AGC_MIN, "inconsistent AGC setting");
   float gain;
   if (AGC_AVG) {  // do automatic gain control based on exponential averaging
      float lastheight = t->v_lasttop - t->v_lastbot; // last peak-to-peak height
      if (lastheight > 0 && lastheight < t->v_avg_height) { // if it's smaller than average  *** DO THIS ALWAYS??
         gain = t->v_avg_height / lastheight;  			// the new gain
         gain = AGC_ALPHA * gain + (1 - AGC_ALPHA)*t->agc_gain;  // exponential smoothing with previous values
         if (gain > AGC_MAX) gain = AGC_MAX;
         move_threshold /= gain; //  reduce the threshold proportionately
         t->agc_gain = gain;
         if (gain > t->max_agc_gain) t->max_agc_gain = gain; } }
   if (AGC_MIN) {  // do automatic gain control based on the minimum of the last n peak-to-peak voltages
      float lastheight = t->v_lasttop - t->v_lastbot; // last peak-to-peak height
      if (lastheight > 0) {
         t->v_heights[t->heightndx] = lastheight; // add the new height to the window
         if (++t->heightndx >= AGC_WINDOW) t->heightndx = 0;
         float minheight = 99;
         for (int i = 0; i < AGC_WINDOW; ++i) if (t->v_heights[i] < minheight) minheight = t->v_heights[i];
         assert(minheight < 99, "bad minimum peak-to-peak voltage");
         if (minheight < t->v_avg_height) { // if min peak-to-peak voltage is lower than during preamble
            gain = t->v_avg_height / minheight;  // what gain we should use
            if (gain > AGC_MAX) gain = AGC_MAX;
            move_threshold /= gain;  // reduce threshold
            t->agc_gain = gain;
            if (gain > t->max_agc_gain) t->max_agc_gain = gain; } } }
   t->last_move_threshold = move_threshold; }

void adjust_clock(struct clkavg_t *c, float delta) {  // update the bit clock speed estimate
   int clk_window = parmsetsptr[block.parmset].clk_window;
   float clk_alpha = parmsetsptr[block.parmset].clk_alpha;
   if (clk_window > 0) { // *** STRATEGY 1: do moving-window averaging
      float olddelta = c->t_bitspacing[c->bitndx]; // save value going out of average
      c->t_bitspacing[c->bitndx] = delta; // insert new value
      if (++c->bitndx >= clk_window) c->bitndx = 0; // circularly increment the index
      c->t_bitspaceavg += (delta - olddelta) / clk_window; // update moving average
   }
   else if (clk_alpha > 0) { // *** STRATEGY 2: do exponential weighted averaging
      c->t_bitspaceavg = // exponential averaging of clock rate
         clk_alpha * delta // weighting of new value
         + (1 - clk_alpha) * c->t_bitspaceavg; // weighting of old values
   }
   else // *** STRATEGY 3: use a constant instead of averaging
      c->t_bitspaceavg = mode == PE ? BIT_SPACING : nrzi.clkavg.t_bitspaceavg; }

/*****************************************************************************************************************************
Routines for 1600 BPI Phase Encoding (PE)
******************************************************************************************************************************/

void pe_end_of_block(void) { // All/most tracks have just become idle. See if we accumulated a data block, a tape mark, or junk
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding

   // a tape mark is bizarre:
   //  -- 80 or more flux reversals (but no data bits) on tracks 0, 2, 5, 6, 7, and P
   //  -- no flux reversals (DC erased) on tracks 1, 3, and 4
   // We actually allow a couple of data bits because of weirdness when the flux transitions.stop
   if (  trkstate[0].datacount <= 2 && trkstate[0].peakcount > 75 &&
         trkstate[2].datacount <= 2 && trkstate[2].peakcount > 75 &&
         trkstate[5].datacount <= 2 && trkstate[5].peakcount > 75 &&
         trkstate[6].datacount <= 2 && trkstate[6].peakcount > 75 &&
         trkstate[7].datacount <= 2 && trkstate[7].peakcount > 75 &&
         trkstate[8].datacount <= 2 && trkstate[8].peakcount > 75 &&
         trkstate[1].peakcount <= 2 &&
         trkstate[3].peakcount <= 2 &&
         trkstate[4].peakcount <= 2
      ) { // got a tape mark
      result->blktype = BS_TAPEMARK;
      return; }

   // to be a valid data block, we remove the postammble and check that all tracks have the same number of bits

   float avg_bit_spacing = 0;
   result->minbits=MAXBLOCK;
   result->maxbits=0;
   //for (int i=60; i<120; ++i)
   //    rlog("%3d: %02X, %c, %d\n", i, data[i]>>1, EBCDIC[data[i]>>1], data[i]&1);

   for (int trk=0; trk<NTRKS; ++trk) { // process postable bits on all tracks
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (t->t_lastbit - t->t_firstbit) / t->datacount;
      //dlog("trk %d firstbit at %.7lf, lastbit at %.7lf, avg spacing %.2f\n", trk, t->t_firstbit, t->t_lastbit, avg_bit_spacing*1e6);
      int postamble_bits;
      if (t->datacount > 0) {
         for (postamble_bits=0; postamble_bits<=MAX_POSTAMBLE_BITS; ++postamble_bits) {
            --t->datacount; // remove one bit
            if ((data_faked[t->datacount] & (0x100>>trk)) != 0) { // if the bit we removed was faked,
               assert(block.results[block.parmset].faked_bits>0, "bad fake data count on trk %d at %.7lf", trk, timenow);
               --block.results[block.parmset].faked_bits;  // then decrement the count of faked bits
               dlog("   remove fake bit %d on track %d\n", t->datacount, trk); }
            // weird stuff goes on as the signal dies at the end of a block, so we ignore the last few data bits.
            if (postamble_bits > IGNORE_POSTAMBLE &&  	// if we've ignored the last few postamble bits
                  (data[t->datacount] & (0x100>>trk)) != 0)	// and we just passed a "1"
               break;  								// then we've erased the postable and are done
         }
         if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain;
         dlog("trk %d had %d postamble bits, max AGC %5.2f\n", trk, postamble_bits, t->max_agc_gain); }
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount; }
   result->avg_bit_spacing = avg_bit_spacing/NTRKS;

   if (result->maxbits == 0) {  // leave result-blktype == BS_NONE
      dlog("   ignoring noise block\n"); }
   else {
      if (result->minbits != result->maxbits) {  // different number of bits in different tracks
         show_track_datacounts("*** malformed block");
         result->blktype = BS_MALFORMED; }
      else {
         result->blktype = BS_BLOCK; }
      result->vparity_errs = 0;
      for (int i=0; i<result->minbits; ++i) // count parity errors
         if (parity(data[i]) != 1) ++result->vparity_errs;
      result->errcount = result->vparity_errs; } }

void pe_addbit (struct trkstate_t *t, byte bit, bool faked, double t_bit) { // we encountered a data bit transition
   if(bit) TRACE(data,high) else TRACE(data,low);
   if (faked) TRACE(fakedata,yes);
   TRACE(datedg,high); 	// data edge
   TRACE(clkwindow,high);	// start the clock window
   if (t->t_lastbit == 0) t->t_lastbit = t_bit - BIT_SPACING; // start of preamble  FIX? TEMP?
   if (t->datablock) { // collecting data
      if (t->trknum == 0 || t->trknum == 4) dlog("trk %d add %d to %3d bytes at %.7lf, V=%.5f, AGC=%.2f\n", t->trknum, bit, t->datacount, t_bit, t->v_now, t->agc_gain);
      t->lastdatabit = bit;
      if (!t->idle && !faked) { // adjust average clock rate based on inter-bit timing
         float delta = t_bit - t->t_lastbit;
         adjust_clock(&t->clkavg, delta);
         t->t_clkwindow = t->clkavg.t_bitspaceavg / 2 * parmsetsptr[block.parmset].clk_factor; }
      t->t_lastbit = t_bit;
      if (t->datacount == 0) t->t_firstbit = t_bit; // record time of first bit in the datablock
      uint16_t mask = 0x100 >> t->trknum;  // update this track's bit in the data array
      data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
      data_faked[t->datacount] = faked ? data_faked[t->datacount] | mask : data_faked[t->datacount] & ~mask;
      if (faked) ++block.results[block.parmset].faked_bits;
      data_time[t->datacount] = t_bit;
      if (t->datacount < MAXBLOCK) ++t->datacount; } }

void pe_top (struct trkstate_t *t) {  // local maximum: end of a positive flux transition
   t->manchdata = 1;
   if (t->datablock) { // inside a data block or the postamble
      bool missed_transition = (t->t_top + t->t_pulse_adj) - t->t_lastpeak > t->t_clkwindow; // missed a half-bit transition?
      if (!t->clknext // if we're expecting a data transition
            || missed_transition) { // or we missed a clock transition
         pe_addbit (t, 1, false, t->t_top);  // then we have new data '1'
         t->clknext = true; }
      else { // this was a clock transition
         TRACE(clkedg,high);
         t->clknext = false; }
      t->t_pulse_adj = ((t->t_top - t->t_lastpeak) - t->clkavg.t_bitspaceavg / (missed_transition ? 1 : 2)) * parmsetsptr[block.parmset].pulse_adj_amt;
      adjust_agc(t); }
   else { // !datablock: inside the preamble
      if (t->peakcount > MIN_PREAMBLE	// if we've seen at least 35 zeroes
            && t->t_top - t->t_lastpeak > t->t_clkwindow) { // and we missed a clock
         t->datablock = true;	// then this 1 means data is starting (end of preamble)
         t->v_avg_height /= t->v_avg_count; // compute avg peak-to-peak voltage
         assert(t->v_avg_height>0, "avg peak-to-peak voltage isn't positive");
         dlog("trk %d start data at %.7lf, AGC %.2f, clk window %lf usec, avg peak-to-peak %.2fV\n", //
              t->trknum, timenow, t->agc_gain, t->t_clkwindow*1e6, t->v_avg_height); }
      else { // stay in the preamble
         t->clknext = false; // this was a clock; data is next
         if (t->peakcount>=AGC_STARTBASE && t->peakcount<=AGC_ENDBASE) { // accumulate peak-to-peak voltages
            t->v_avg_height += t->v_top - t->v_bot;
            ++t->v_avg_count;
            t->v_heights[t->heightndx] = t->v_top - t->v_bot;
            if (++t->heightndx >= AGC_WINDOW) t->heightndx = 0; } } } }

void pe_bot (struct trkstate_t *t) { // local minimum: end of a negative flux transition
   t->manchdata = 0;
   if (t->datablock) { // inside a data block or the postamble
      bool missed_transition = (t->t_bot + t->t_pulse_adj) - t->t_lastpeak > t->t_clkwindow; // missed a half-bit transition?
      if (!t->clknext // if we're expecting a data transition
            || missed_transition) { // or we missed a clock transition
         pe_addbit (t, 0, false, t->t_bot);  // then we have new data '0'
         t->clknext = true; }
      else { // this was a clock transition
         TRACE(clkedg,high);
         t->clknext = false; }
      t->t_pulse_adj = ((t->t_bot - t->t_lastpeak) - t->clkavg.t_bitspaceavg / (missed_transition ? 1 : 2)) * parmsetsptr[block.parmset].pulse_adj_amt;
      adjust_agc(t); }
   else { // inside the preamble
      t->clknext = true; // force this to be treated as a data transition; clock is next
   } }

/*****************************************************************************************************************************
Routines for 800 BPI NRZI encoding
******************************************************************************************************************************/

void nrzi_end_of_block(void) {
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   float avg_bit_spacing = 0;
   nrzi.datablock = false;
   result->minbits = MAXBLOCK;
   result->maxbits = 0;
   for (int trk = 0; trk < NTRKS; ++trk) { //
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (t->t_lastbit - t->t_firstbit) / t->datacount;
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount;
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain; }
   result->avg_bit_spacing = avg_bit_spacing / NTRKS;
   dlog("end_of_block, min %d max %d, avgbitspacing %f, at %.7lf\n", result->minbits, result->maxbits, result->avg_bit_spacing*1e6, timenow);
   if (result->maxbits == 0) {  // leave result-blktype == BS_NONE
      dlog("   ignoring noise block\n"); }
   else {
      if (result->minbits != result->maxbits) {  // different number of bits in different tracks
         show_track_datacounts("*** malformed block");
         result->blktype = BS_MALFORMED; }
      else { // well-formed block
         if (result->minbits == 3  // if it's a 3-byte block
               && data[0] == 0x26 && data[1] == 0 && data[2] == 0x26) // with trks 3/6/7, then nothing, then 3/6/7
            result->blktype = BS_TAPEMARK; // then it's the bizarre tapemark
         else result->blktype = BS_BLOCK; }
      result->vparity_errs = 0;
      int crc = 0;
      int lrc = 0;
      if (result->blktype != BS_TAPEMARK && result->minbits > 2) {
         for (int i = 0; i < result->minbits - 2; ++i) {  // count parity errors, and check the CRC/LRC at the end
            if (parity(data[i]) != 1) ++result->vparity_errs;
            lrc ^= data[i];
            crc ^= data[i]; // C0..C7,P  (See IBM Form A22-6862-4)
            if (crc & 2) crc ^= 0xf0; // if P will become 1 after rotate, invert what will go into C2..C5
            int lsb = crc & 1; // rotate all 9 bits
            crc >>= 1;
            if (lsb) crc |= 0x100; }
         crc ^= 0x1af; // invert all except C2 and C4; note that the CRC could be zero if the number of data bytes is odd
         lrc ^= crc;  // LRC inlcudes the CRC (the manual doesn't say that!)
         result->crc = data[result->minbits - 2];
         result->lrc = data[result->minbits - 1];
         result->errcount = result->vparity_errs;
         if (crc != result->crc) {
            result->crc_bad = true;
            ++result->errcount; }
         if (lrc != result->lrc) {
            result->lrc_bad = true;
            ++result->errcount; }
         dlog("crc is %03X, should be %03X\n", result->crc, crc);
         dlog("lrc is %03X, should be %03X\n", result->lrc, lrc);
         result->minbits -= 2;  // don't include CRC and LRC in the data
         result->maxbits -= 2; }
      interblock_expiration = timenow + NRZI_IBG_SECS;  // ignore data for a while until we're well into the IBG
   } }

void nrzi_addbit(struct trkstate_t *t, byte bit, double t_bit) { // add a NRZI bit
   if (bit) TRACE(data, high) else TRACE(data, low);
   /*if (nrzi.post_counter)*/ dlog("addbit at midbit %d at %.7lf, add %d to trk %d datacount %d\n", nrzi.post_counter, timenow, bit, t->trknum, t->datacount);
   t->t_lastbit = t_bit;
   if (t->datacount == 0) {
      t->t_firstbit = t_bit; // record time of first bit in the datablock
      t->v_avg_height = 2 * (t->v_bot + t->v_top); // avg peak-to-peak is twice the first top or bottom
      t->max_agc_gain = t->agc_gain; }
   if (!nrzi.datablock) { // this is the begining of data for this block
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg;
      nrzi.datablock = true; }
   if (trkstate[0].datacount > 900) dlog("trk %d at %.7lf, lastpeak %.7lf, add %d to %d bytes at %.7lf, bitspacing %.2f\n", //
                                            t->trknum, timenow, t->t_lastpeak, bit, t->datacount, t_bit, nrzi.clkavg.t_bitspaceavg*1e6);
   uint16_t mask = 0x100 >> t->trknum;  // update this track's bit in the data array
   data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
   data_time[t->datacount] = t_bit;
   if (t->datacount < MAXBLOCK) ++t->datacount;
   if (nrzi.post_counter > 0 && bit) { // we're at the end of a block and get a one: must be LRC or CRC
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg; // reset when we thought the last clock was
   } }

void nrzi_deletebits(void) {
   for (int trknum = 0; trknum < NTRKS; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      assert(t->datacount > 0, "bad NRZI data count");
      --t->datacount; } }

void nrzi_bot(struct trkstate_t *t) { // local minimum: end of a negative flux transition
//   dlog("trk %d bot at %.7f\n", t->trknum, t->t_bot);
   nrzi_addbit(t, 1, t->t_bot);  // add a data 1
   t->hadbit = true;
   adjust_agc(t); }

void nrzi_top(struct trkstate_t *t) {  // local maximum: end of a positive flux transition
//   dlog("trk %d top at %.7f\n", t->trknum, t->t_top);
   nrzi_addbit(t, 1, t->t_top); // add a data 1
   t->hadbit = true;
   if (t->peakcount >= AGC_STARTBASE && t->peakcount <= AGC_ENDBASE) { // accumulate initial peak-to-peak voltages
      t->v_avg_height += t->v_top - t->v_bot;
      ++t->v_avg_count;
      t->v_heights[t->heightndx] = t->v_top - t->v_bot;
      if (++t->heightndx >= AGC_WINDOW) t->heightndx = 0; }
   else if (t->peakcount > AGC_ENDBASE) { // we're beyond the first set of peak and have some peak-to-peak history
      if (t->v_avg_count) { // if the is the first time we've gone beyond
         t->v_avg_height /= t->v_avg_count; // then compute avg peak-to-peak voltage
         t->v_avg_count = 0; }
      else adjust_agc(t); // otherwise start adjusting AGC
   } }

void nrzi_midbit(void) { // we're in between NRZI bit times
   int numbits = 0;
   double avgpos = 0;
   trace_midbit = trace_midbit_high;
   //if (nrzi.post_counter) dlog("midbit %d at %.7lf\n", nrzi.post_counter, timenow);
   if (nrzi.post_counter == 0 // if we're not at the end of the block yet
         || nrzi.post_counter == 4 // or we're 1.5 bit times past the CRC
         || nrzi.post_counter == 8 // or we're 1.5 bit times past the LRC
      ) {  // then process this interval
      for (int trknum = 0; trknum < NTRKS; ++trknum) {
         struct trkstate_t *t = &trkstate[trknum];
         if (!t->hadbit) { // if this track had no transition at the last clock
            nrzi_addbit(t, 0, timenow - nrzi.clkavg.t_bitspaceavg * NRZI_MIDPOINT); // so add a zero bit at approximately the last clock time
         }
         else { // there was a transition on this track: accumulate the average position of the clock time
            avgpos += t->t_lastpeak;
            t->hadbit = false;
            ++numbits; } } // for all tracks

      //if (nrzi.post_counter) dlog("  %d transitions\n", numbits);
      if (numbits > 0) { // at least one track had a flux transition at the last clock
         avgpos /= numbits;
         float delta = avgpos - nrzi.t_lastclock;
         //if (trkstate[0].datacount > 900) dlog("adjust clk at %.7lf with delta %.2fus into avg %.2fus, avg pos %.7lf, lastclk %.7lf\n", //
         //                                         timenow, delta*1e6, nrzi.clkavg.t_bitspaceavg*1e6, avgpos, nrzi.t_lastclock); //
         if (nrzi.post_counter == 0) adjust_clock(&nrzi.clkavg, delta);  // adjust the clock rate based on the average position
         nrzi.t_lastclock = avgpos;  // use it as the last clock position
         if (nrzi.post_counter) ++nrzi.post_counter; // we in the post-block: must have been CRC or LRC
      }

      else { // there were no transitions, so we must be at the end of the block
         // The CRC is supposed to be after 3 quiet bit times (post_counter == 3), and then
         // the LRC is supposed to be after another 3 quiet bit times (post counter == 7).
         // The problem is that they seem often to be delayed by about half a bit, which screws up our clocking. So we:
         //  1. don't update the clock speed average during this post-data time.
         //  2. Estimate the last clock time based on any CRC or LRC 1-bit transitions we see.
         //  3. Wait until one bit time later to accmulate the 0-bits for the CRC/LRC, ie post_counter 4 and 8.
         nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // compute a hypothetical last clock position
         if (nrzi.post_counter == 0) nrzi_deletebits(); // delete all the zero bits we just added
         ++nrzi.post_counter; } }

   else if (nrzi.post_counter) {
      ++nrzi.post_counter; // we're not processing this interval; just count it
      nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // and advance the hypothetical last clock position
   }

   if (nrzi.post_counter > 8) nrzi_end_of_block();
   //if (trkstate[0].datacount > 900) dlog("midbit at %.7lf, %d transitions, lastclock %.7lf, trk0 %d bytes, post_ctr %d\n", //
   //                                         timenow, numbits, nrzi.t_lastclock, trkstate[0].datacount, nrzi.post_counter); //
}
/*****************************************************************************************************************************
Analog sample processing
******************************************************************************************************************************/

void end_of_block(void) {
   if (mode == PE) pe_end_of_block();
   else if (mode == NRZI) nrzi_end_of_block();
   else fatal("GCR end of block not implemented"); }

int choose_number_of_faked_bits(struct trkstate_t *t) {
   // We try various algorithms for choosing how many bits to add when there is a dropout in a track.
   // None of these is great. Have any other ideas?
   int numbits, strategy;
   int trknum = t->trknum;
   //for (strategy=1; strategy<=4; ++strategy) { // compute and display all strategies
   strategy=1; // MAKE A CHOICE HERE. Should we add it to the parameter block?
   switch(strategy) { // which of the following ideas to try
   case 1: // The number of bits is based on the time between now and the last bit, and the avg bit spacing.
      // It won't work if the clock is drifting
      numbits = (timenow - t->t_lastbit /* + t->t_bitspaceavg/2*/) / t->clkavg.t_bitspaceavg;
      break;
   case 2: //  Add enough bits to give this track the same number as the minimum of
      //  any track which is still getting data.
      numbits = INT_MAX;
      for (int i=0; i<NTRKS; ++i) {
         if (i!=trknum && !trkstate[i].idle && trkstate[i].datacount < numbits) numbits = trkstate[i].datacount; }
      if (numbits != INT_MAX && numbits > t->datacount) numbits -= t->datacount;
      else numbits = 0;
      break;
   case 3:	//  Add enough bits to give this track the same number as the maximum of
      //  any track which is still getting data.
      numbits = 0;
      for (int i=0; i<NTRKS; ++i) {
         if (i!=trknum && !trkstate[i].idle && trkstate[i].datacount > numbits) numbits = trkstate[i].datacount; }
      numbits -= t->datacount;
      break;
   case 4: // Add enough bits to make this track as long as the average of the other tracks
      numbits = 0;
      int numtrks = 0;
      for (int i=0; i<NTRKS; ++i) {
         if (i!=trknum && !trkstate[i].idle) {
            numbits += trkstate[i].datacount;
            ++numtrks; } }
      numbits = numbits/numtrks - t->datacount;
      break;
   default: fatal("bad choose_bad_bits strategy", ""); }
   //dlog("  strategy %d would add %d bits at %.7lf\n", strategy, numbits, timenow); }
   assert(numbits>0, "choose_bad_bits bad count");
   return numbits; }

//
// Process one voltage sample with data for all tracks
// Return with the status of the block we're working on.
//
enum bstate_t process_sample(struct sample_t *sample) {
   float deltaT, move_threshold;

   if (sample == NULLP) { // special code for endfile
      end_of_block();
      return BS_NONE; }

   deltaT = sample->time - timenow;  //(incorrect for the first sample)
   timenow = sample->time;

   if (interblock_expiration && timenow < interblock_expiration) // if we're waiting for an IBG to really start
      goto exit;

   if (nrzi.datablock && timenow > nrzi.t_lastclock + (1 + NRZI_MIDPOINT)*nrzi.clkavg.t_bitspaceavg) {
      nrzi_midbit();  // do NRZI mid-bit computations
   }

   for (int trknum = 0; trknum < NTRKS; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      t->v_now = sample->voltage[trknum];

      // force voltage near zero to be zero
      float zero_band = parmsetsptr[block.parmset].zero_band / t->agc_gain;
      if (t->v_now < zero_band && t->v_now > -zero_band) t->v_now = 0;

      //dlog("trk %d state %d voltage %f at %.7lf\n", trknum, t->astate, t->v_now, timenow);
      TRACE(peak, not);
      TRACE(clkedg, low);
      TRACE(datedg, low);
      TRACE(fakedata, no);
      if (timenow - t->t_lastbit > t->t_clkwindow) {
         TRACE(clkwindow, low); }
      //if (timenow > 7.8904512 && trknum==1) rlog("time %.7lf, lastbit %.7lf, delta %lf, clkwindow %lf\n", //
      //    timenow, t->t_lastbit, (timenow-t->t_lastbit)*1e6, t->t_clkwindow*1e6);
      if (t->manchdata) TRACE(manch, high) else TRACE(manch, low);

      if (num_samples == 0)  t->v_lastpeak = t->v_now;
      move_threshold = t->last_move_threshold;
      if (t->astate == AS_IDLE) { // idling, waiting for the start of a flux transition
         t->v_top = t->v_bot = t->v_now;
         t->t_top = t->t_bot = timenow;
         if (t->v_now > t->v_lastpeak + move_threshold) t->astate = AS_UP;
         if (t->v_now < t->v_lastpeak - move_threshold) t->astate = AS_DOWN;
         if (t->astate != AS_IDLE) {
            dlog("trk %d not idle, %d idle, going %s at %.7f, AGC %.2f, thresh %.3f, v_now %f, v_lastpeak %f, bitspaceavg %.2f\n", //
                 trknum, num_trks_idle - 1, t->astate == AS_UP ? "up" : "down", timenow, t->agc_gain, move_threshold, t->v_now, t->v_lastpeak, t->clkavg.t_bitspaceavg*1e6);
            if (mode == PE) t->t_lastpeak = timenow;  // TEMP problem? it isn't really a peak!
            --num_trks_idle;
            t->moving = true;
            t->idle = false;
            TRACE(clkdet, high);
            if (FAKE_BITS && mode != NRZI && t->datablock && t->datacount > 1) {
               //  For PE and GCR, if transitions have returned within a data block after a gap.
               //  Add extra data bits that are same as the last bit before the gap started,in an
               //  attempt to keep this track in sync with the others. .
               int numbits = choose_number_of_faked_bits(t);
               if (numbits > 0) {
                  dlog("trk %d adding %d fake bits to %d bits at %.7lf, lastbit at %.7lf, bitspaceavg=%.2f\n", //
                       trknum, numbits, t->datacount, timenow, t->t_lastbit, t->clkavg.t_bitspaceavg*1e6);
                  if (DEBUG) show_track_datacounts("*** before adding bits");
                  while (numbits--) pe_addbit(t, t->lastdatabit, true, timenow);
                  t->t_lastbit = 0; // don't let the bitspacing averaging algorithm work on these bits
                  if (t->lastdatabit == 0 && t->astate == AS_DOWN || t->lastdatabit == 1 && t->astate == AS_UP) {
                     // the first new peak will be a data bit, when it comes
                     t->clknext = false; }
                  else { // the first new peak will be a clock bit, when it comes
                     t->clknext = true;
                     TRACE(clkwindow, low); } } }
            goto new_maxmin; } } // AS_IDLE

      else { // going up or down: check for having waited too long for a peak
         if (mode == PE && t->t_lastpeak != 0 && timenow - t->t_lastpeak > t->clkavg.t_bitspaceavg * IDLE_FACTOR) {
            t->astate = AS_IDLE;
            t->v_lastpeak = t->v_now;
            t->idle = true;
            TRACE(clkdet, low);
            dlog("trk %d became idle at %.7lf, %d idle, AGC %.2f, threshold %.3f, last peak at %.7lf, bitspaceavg %.2f usec, datacount %d\n", //
                 trknum, timenow, num_trks_idle + 1, t->agc_gain, move_threshold, t->t_lastpeak, t->clkavg.t_bitspaceavg*1e6, t->datacount);
            if (++num_trks_idle >= IDLE_TRK_LIMIT) {
               end_of_block(); } } }

new_maxmin:
      if (t->astate == AS_UP) {  // we are moving up towards a maximum
         if (t->v_now > t->v_lastpeak + move_threshold) // if we've really started moving
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
               t->t_top = timenow; }
            else if (deltaV >= 0) {  // moved up by a little, or stayed the same
               t->v_top = t->v_now;
               t->t_top = timenow - deltaT / 2; // we could do some interpolation based on how "little" is compared to PEAK_THRESHOLD
               //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated top going up  at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_top);
            }
            else { // moving down
               if (deltaV >= -PEAK_THRESHOLD) { // move down, but only by a little
                  //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("  timenow=%.9f, t_top=%.9f\n", timenow, t->t_top);
                  if (t->t_top < timenow - deltaT - EPSILON_T) t->t_top = timenow - deltaT; // erase previous interpolation
                  else t->t_top = timenow - deltaT / 2;
                  //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated top going down at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_top);
               } // else moving down by a lot: keep previous top time
               TRACE(peak, top); // record a peak
               ++t->peakcount;
               if (mode == PE) pe_top(t);
               else if (mode == NRZI) nrzi_top(t);
               else fatal("GCR not supported yet");
               t->v_lasttop = t->v_top;
               t->v_lastpeak = t->v_top;
               t->t_lastpeak = t->t_top;
               t->astate = AS_DOWN; // start moving down
               t->moving = false; // but haven't started a big move yet
               t->v_bot = t->v_now; // start tracking the bottom on the way down
            } } }

      if (t->astate == AS_DOWN) {  // we are moving down towards a minimum
         if (t->v_now < t->v_lastpeak - move_threshold) // if we've really started moving
            t->moving = true;
         if (t->moving) {
            // The above four cases apply, mutatis mutandis, here as well.
            float deltaV = t->v_now - t->v_bot;
            if (deltaV < -PEAK_THRESHOLD) { // still moving down by a lot
               t->v_bot = t->v_now; // just record the new miminum
               t->t_bot = timenow; }
            else if (deltaV <= 0) {  // moved down by a little, or stayed the same
               t->v_bot = t->v_now;
               t->t_bot = timenow - deltaT / 2; // we could do some interpolation based on how "little" is compared to PEAK_THRESHOLD
               //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated bot going down at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_bot);
            }
            else { // moving up
               if (deltaV <= PEAK_THRESHOLD) { // move up, but only by a little
                  if (t->t_bot < timenow - deltaT - EPSILON_T) t->t_bot = timenow - deltaT; // erase previous interpolation
                  else t->t_bot = timenow - deltaT / 2;
                  //if (t->trknum==0 && t->datacount>1408 && t->datacount<1413) dlog("trk %d interpolated bot going up   at byte %d to time %.7f\n", t->trknum, t->datacount, t->t_bot);
               } // else moving up by a lot: keep previous bottom time
               TRACE(peak, bot); // record a peak
               ++t->peakcount;
               if (mode == PE) pe_bot(t);
               else if (mode == NRZI) nrzi_bot(t);
               else fatal("GCR not supported yet");
               t->v_lastbot = t->v_bot;
               t->v_lastpeak = t->v_bot;
               t->t_lastpeak = t->t_bot;
               t->astate = AS_UP; // start moving up
               t->moving = false; // but haven't started a big move yet
               t->v_top = t->v_now; // start tracking the top on the way up
            } } }

   } // for tracks

#if DEBUG && TRACEFILE
   if (!tracef) {
      char filename[MAXPATH];
      sprintf(filename, "%s\\trace.csv", basefilename);
      assert(tracef = fopen(filename, "w"), "can't open trace file \"%s\"", filename);
#if MULTITRACK
      fprintf(tracef, "time, trk0, trk1, trk2, trk3, trk4, trk5, trk6, trk7, trk8\n");
#else
      fprintf(tracef, "TRK%d time, deltaT, datacount, v trc, peak, manch, clkwind, data, clkedg, datedg, clkdet,"
              "fakedata, midbit, AGC gain, move thresh, clkwind, bitspaceavg, peak deltaT, lastheight\n", TRACETRK);
#endif
   }

   // Put special tests here for turning the trace on, depending on what anomaly we're looking at...
   //if (trkstate[TRACETRK].datacount==900 && !trace_done) trace_on = true;
   //if (timenow > 0 && !trace_done) trace_on = true;
   if (trkstate[TRACETRK].datacount > 550) trace_on = true;
   //if (trkstate[TRACETRK].datacount > 22) trace_on = false;
   //if (num_samples == 8500) trace_on = true;
   //if (trkstate[3].datacount > trkstate[0].datacount+1) trace_on = true;
   if (trace_on) {
      if (trace_lines < 100) { // limit on how much trace data to collect
#if MULTITRACK // create multi-track analog trace
         fprintf(tracef, "%.8lf, ", timenow);
         for (int i = 0; i < NTRKS; ++i) fprintf(tracef, "%.5f, ", trkstate[i].v_now);
         fprintf(tracef, "\n");
#else // create debugging trace
         static double last_peak_time = 0;
         float peak_delta_time = 0;
         static double timestart = 0;
         if (timestart == 0) timestart = last_peak_time = timenow;
         if (trace_peak != trace_peak_not) { //compute time from last peak
            peak_delta_time = timenow - last_peak_time;
            last_peak_time = timenow; }
         fprintf(tracef, "%.8lf, %.2lf, %d, %f, %f, ",
                 timenow, (timenow - timestart)*1e6, trkstate[TRACETRK].datacount, sample->voltage[TRACETRK]);
         fprintf(tracef, "%f, %f, %f, %f, %f, %f, %f, %f, ",
                 trace_peak, trace_manch, trace_clkwindow, trace_data, trace_clkedg, trace_datedg, trace_clkdet, trace_fakedata, trace_midbit);
         fprintf(tracef, "%.2f, %.5f, %.2f, %.2f, %.2f, %.2f\n", //
                 trkstate[TRACETRK].agc_gain, trkstate[TRACETRK].last_move_threshold, trkstate[TRACETRK].t_clkwindow*1e6,
                 trkstate[TRACETRK].clkavg.t_bitspaceavg*1e6, peak_delta_time*1e6, trkstate[TRACETRK].v_lasttop - trkstate[TRACETRK].v_lastbot);
#endif
         trace_midbit = trace_midbit_low;
         ++trace_lines; }
      else {
         trace_on = false;
         trace_done = true; } }
#endif // DEBUG && TRACEFILE

exit:
   ++num_samples;
   return block.results[block.parmset].blktype;  // what block type we found, if any
}

//*

