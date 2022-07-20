//file: decode_pe.c
/******************************************************************************

   decode routines specific to 1600 BPI PE tape format

---> See readtape.c for the merged change log.

*******************************************************************************
Copyright (C) 2018, Len Shustek

The MIT License (MIT): Permission is hereby granted, free of charge, to any
person obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#include "decoder.h"

/*****************************************************************************************************************************
   Routines for 1600 BPI Phase Encoding (PE)
******************************************************************************************************************************/

void pe_end_of_block(void) { // All/most tracks have just become idle. See if we accumulated a data block, a tape mark, or junk
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   if (block.endblock_done) return;
   block.endblock_done = true;

   // a tape mark is bizarre:
   //  -- 80 or more flux reversals (but no data bits) on tracks 0, 2, 5, 6, 7, and P
   //  -- no flux reversals (DC erased) on tracks 1, 3, and 4
   // We actually allow a couple of data bits because of weirdness when the flux transitions stop
   if (trkstate[0].datacount <= 2 && trkstate[0].peakcount > 75 &&
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

   // to extract a valid data block, we remove the postamble and check that all tracks have the same number of bits
   float avg_bit_spacing = 0;
   result->minbits = MAXBLOCK;
   result->maxbits = 0;

   for (int trk = 0; trk < ntrks; ++trk) { // process postamble bits on all tracks
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (float)(t->t_lastbit - t->t_firstbit) / t->datacount;
      //dlog("trk %d firstbit at %.8lf, lastbit at %.8lf, avg spacing %.2f\n", trk, t->t_firstbit, t->t_lastbit, avg_bit_spacing*1e6);
      int postamble_bits;
      if (t->datacount > 0) {
         for (postamble_bits = 0; postamble_bits <= PE_MAX_POSTBITS; ++postamble_bits) {
            --t->datacount; // remove one bit
            if ((data_faked[t->datacount] & (1 << (ntrks - 1 - trk))) != 0) { // if the bit we removed was faked,
               assert(block.results[block.parmset].corrected_bits > 0, "bad fake data count on trk %d at %.8lf", trk, timenow);
               --block.results[block.parmset].corrected_bits;  // then decrement the count of faked bits
               dlog("   remove fake bit %d on track %d\n", t->datacount, trk); //
            }
            // weird stuff goes on as the signal dies at the end of a block, so we ignore the last few data bits.
            if (postamble_bits > PE_IGNORE_POSTBITS &&  	// if we've ignored the last few postamble bits
                  (data[t->datacount] & (1 << (ntrks - 1 - trk))) != 0)	// and we just passed a "1"
               break;  								// then we've erased the postable and are done
         }
         if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain;
         if (result->alltrk_min_agc_gain > t->min_agc_gain) result->alltrk_min_agc_gain = t->min_agc_gain;
         dlogtrk("trk %d had %d postamble bits, max AGC %5.2f, datacount %d\n", trk, postamble_bits, t->max_agc_gain, t->datacount); }
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount; }
   result->avg_bit_spacing = avg_bit_spacing / ntrks;
   set_expected_parity(result->maxbits);

   if (result->maxbits == 0) {
      dlog("   detected noise block at %.8lf\n", timenow);
      if (!doing_density_detection)
         result->blktype = BS_NOISE;
      // If we're doing density detection, most everything (especially for PE) looks like noise because bitspaceavg=0
      // So we don't end the block here; eventually estden_transition() will force that to happen.
   }
   else {
      result->blktype = BS_BLOCK;
      interblock_counter = (int)(PE_IBG_SECS / sample_deltat);  // ignore data for a while until we're well into the IBG
      if (result->minbits != result->maxbits) {  // different number of bits in different tracks
         // Unlike NRZI and GCR, we allow blocks with mismatched tracks to be written. Might be ok, sorta.
         if (DEBUG) show_track_datacounts("*** trkmismatched block");
         result->track_mismatch = result->maxbits - result->minbits; }
      result->vparity_errs = 0;
      for (int i = 0; i < result->minbits; ++i) // count parity errors
         if (parity(data[i]) != expected_parity) ++result->vparity_errs; } }

void pe_addbit (struct trkstate_t *t, byte bit, bool faked, double t_bit) { // we encountered a data bit transition
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   //if (faked) TRACE(fakedata,yes);
   TRACE(datedg, t_bit, UPTICK, t); 	// data edge
   if (t->t_lastbit != 0) TRACE(clkwin, t->t_lastbit + t->t_clkwindow, DNTICK, t); // show where the previous clock window ended
   TRACE(clkwin,t_bit, UPTICK, t);	   // start a new clock window
   if (t->t_lastbit == 0) t->t_lastbit = t_bit - 1/(bpi*ips); // start of preamble  FIX? TEMP? Ok?
   if (t->datablock) { // collecting data
      dlogtrk("trk %d add %d to %3d bytes at %.8lf, V=%.5f, AGC=%.2f\n", t->trknum, bit, t->datacount, t_bit, t->v_now, t->agc_gain);
      t->lastdatabit = bit;
      if (!t->idle && !faked) { // adjust average clock rate based on inter-bit timing
         float delta = (float)(t_bit - t->t_lastbit);
         adjust_clock(&t->clkavg, delta, t->trknum);
         t->t_clkwindow = t->clkavg.t_bitspaceavg / 2 * PARM.clk_factor; }
      t->t_lastbit = t_bit;
      if (t->datacount == 0) t->t_firstbit = t_bit; // record time of first bit in the datablock
      uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
      data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
      data_faked[t->datacount] = faked ? data_faked[t->datacount] | mask : data_faked[t->datacount] & ~mask;
      if (faked) ++block.results[block.parmset].corrected_bits;
      data_time[t->datacount] = t_bit;
      if (t->datacount < MAXBLOCK) ++t->datacount; } }

void pe_preamble_peak(struct trkstate_t *t, bool is_top) {
   // process a peak during the preamble, and see if it is a one-bit that starts the data
   if (t->peakcount == 1) {  // the first transition, which is a 0-bit, sets the bit polarity
      t->bit1_up = !is_top; // (normally 1 is up, but might be reversed depending on logic analyzer polarity)
      static warned_polarity = false; // should be in a file-specific structure
      if (!t->bit1_up && !warned_polarity) {
         rlog("*** NOTE: we detected reverse PE signal polarity, but we can handle it\n");
         warned_polarity = true; }
      block.t_blockstart = timenow; } // record the start of the block
   if (t->peakcount > PE_MIN_PREBITS	// if we've seen at least 35 zeroes
         && t->bit1_up == is_top // and this is the direction that could be a one
         && (is_top ? t->t_top : t->t_bot) - t->t_lastpeak > t->t_clkwindow) { // and we missed a clock
      t->datablock = true;	// then this 1 means data is starting (end of preamble)
      t->v_avg_height = t->v_avg_height_sum / t->v_avg_height_count; // compute avg peak-to-peak voltage
      //dlog("trk %d avg peak-to-peak is %.2fV at %.8lf\n", t->trknum, t->v_avg_height, timenow);
      dlogtrk("trk %d starts data at %.8lf tick %.1lf, AGC %.2f, clk window %lf usec, avg peak-to-peak %.2fV\n",
              t->trknum, timenow, TICK(timenow), t->agc_gain, t->t_clkwindow*1e6, t->v_avg_height);
      assert(t->v_avg_height > 0, "avg peak-to-peak voltage isn't positive on trk %d at %.8lf", t->trknum, timenow);   //
   }
   else { // stay in the preamble
      t->clknext = is_top != t->bit1_up;  // set whether data or clock comes next
      if (t->peakcount >= AGC_STARTBASE && t->peakcount <= AGC_ENDBASE) { // accumulate peak-to-peak voltages
         //assert(t->v_top - t->v_bot >= 0, "PE preamble peak height negative (%.3f) on trk %d at time %.8lf\n",
         //   t->v_top - t->v_bot, t->trknum, timenow);
         if (t->v_top > t->v_bot) {
            t->v_avg_height_sum += t->v_top - t->v_bot;
            ++t->v_avg_height_count;
            t->v_heights[t->heightndx] = t->v_top - t->v_bot;
            if (++t->heightndx >= PARM.agc_window) t->heightndx = 0; } } } }

void pe_top (struct trkstate_t *t) {  // local maximum: end of a positive flux transition
   if (t->datablock) { // inside a data block
      if (PEAK_STATS)
         record_peakstat(t->clkavg.t_bitspaceavg, (float)(t->t_top - t->t_lastpeak), t->trknum);
      bool missed_transition = (t->t_top + t->t_pulse_adj) - t->t_lastpeak > t->t_clkwindow; // missed a half-bit transition?
      if (!t->clknext // if we're expecting a data transition
            || missed_transition) { // or we missed a clock transition
         dlogtrk("trk %d add %d top at %.8lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.8lf tick %.1lf, clkwin %.2f uS\n",
                 t->trknum, t->bit1_up, t->t_top, TICK(t->t_top), t->t_pulse_adj*1e6, t->v_top, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         pe_addbit (t, t->bit1_up, false, t->t_top);  // then we have new data
         t->clknext = true; }
      else { // this was a clock transition
         TRACE(clkedg, t->t_top, UPTICK, t);
         TRACE(clkdet, t->t_top, UPTICK, t);
         dlogtrk("trk %d clk   top at %.8lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.8lf tick %.1lf, clkwin %.2f uS\n",
                 t->trknum, t->t_top, TICK(t->t_top), t->t_pulse_adj*1e6, t->v_top, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         t->clknext = false; }
      t->t_pulse_adj = ((float)(t->t_top - t->t_lastpeak) - t->clkavg.t_bitspaceavg / (missed_transition ? 1 : 2)) * PARM.pulse_adj;
      adjust_agc(t); }
   else { // !datablock: we're inside the preamble
      pe_preamble_peak(t, true); //
   } }

void pe_bot (struct trkstate_t *t) { // local minimum: end of a negative flux transition
   if (t->datablock) { // inside a data block or the postamble
      if (PEAK_STATS)
         record_peakstat(t->clkavg.t_bitspaceavg, (float)(t->t_bot - t->t_lastpeak), t->trknum);
      bool missed_transition = (t->t_bot + t->t_pulse_adj) - t->t_lastpeak > t->t_clkwindow; // missed a half-bit transition?
      if (!t->clknext // if we're expecting a data transition
            || missed_transition) { // or we missed a clock transition
         dlogtrk("trk %d add %d bot at %.8lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.8lf tick %.1lf, clkwin %.2f uS\n",
                 t->trknum, !t->bit1_up, t->t_bot, TICK(t->t_bot), t->t_pulse_adj*1e6, t->v_bot, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         pe_addbit (t, !t->bit1_up, false, t->t_bot);  // then we have new data
         t->clknext = true; }
      else { // this was a clock transition
         TRACE(clkedg,t->t_bot, UPTICK, t);
         TRACE(clkdet,t->t_bot, UPTICK, t);
         dlogtrk("trk %d clk   bot at %.8lf tick %.1lf + adj %.2f uS, %.3fV, lastpeak at %.8lf tick %.1lf, clkwin %.2f uS\n",
                 t->trknum, t->t_bot, TICK(t->t_bot), t->t_pulse_adj*1e6, t->v_bot, t->t_lastpeak, TICK(t->t_lastpeak), t->t_clkwindow*1e6);
         t->clknext = false; }
      t->t_pulse_adj = ((float)(t->t_bot - t->t_lastpeak) - t->clkavg.t_bitspaceavg / (missed_transition ? 1 : 2)) * PARM.pulse_adj;
      adjust_agc(t); }
   else { // !datablock: we're inside the preamble
      pe_preamble_peak(t, false); //
   } }


int choose_number_of_corrected_bits(struct trkstate_t *t) {
   // We try various algorithms for choosing how many bits to add when there is a dropout in a track.
   // None of these is great. Have any other ideas?
   int numbits = 0, strategy;
   int trknum = t->trknum;
   //for (strategy=1; strategy<=4; ++strategy) { // compute and display all strategies
   strategy = 1; // MAKE A CHOICE HERE. Should we add it to the parameter block?
   switch (strategy) { // which of the following ideas to try
   case 1: // The number of bits is based on the time between now and the last bit, and the avg bit spacing.
      // It won't work if the clock is drifting
      numbits = (int)((float)(timenow - t->t_lastbit /* + t->t_bitspaceavg/2*/) / t->clkavg.t_bitspaceavg);
      break;
   case 2: //  Add enough bits to give this track the same number as the minimum of
      //  any track which is still getting data.
      numbits = INT_MAX;
      for (int i = 0; i<ntrks; ++i) {
         if (i != trknum && !trkstate[i].idle && trkstate[i].datacount < numbits) numbits = trkstate[i].datacount; }
      if (numbits != INT_MAX && numbits > t->datacount) numbits -= t->datacount;
      else numbits = 0;
      break;
   case 3:	//  Add enough bits to give this track the same number as the maximum of
      //  any track which is still getting data.
      numbits = 0;
      for (int i = 0; i<ntrks; ++i) {
         if (i != trknum && !trkstate[i].idle && trkstate[i].datacount > numbits) numbits = trkstate[i].datacount; }
      numbits -= t->datacount;
      break;
   case 4: // Add enough bits to make this track as long as the average of the other tracks
      numbits = 0;
      int numtrks = 0;
      for (int i = 0; i<ntrks; ++i) {
         if (i != trknum && !trkstate[i].idle) {
            numbits += trkstate[i].datacount;
            ++numtrks; } }
      numbits = numbits / numtrks - t->datacount;
      break;
   default: fatal("bad choose_bad_bits strategy", ""); }
   //dlog("  strategy %d would add %d bits at %.8lf\n", strategy, numbits, timenow); //
   assert(numbits>0, "choose_bad_bits bad count");
   return numbits; }

void pe_generate_fake_bits(struct trkstate_t *t) {
   int numbits = choose_number_of_corrected_bits(t);
   if (numbits > 0) {
      dlog("trk %d adding %d fake bits to %d bits at %.8lf, lastbit at %.8lf, bitspaceavg=%.2f\n", //
           t->trknum, numbits, t->datacount, timenow, t->t_lastbit, t->clkavg.t_bitspaceavg*1e6);
      if (DEBUG) show_track_datacounts("*** before adding bits");
      while (numbits--) pe_addbit(t, t->lastdatabit, true, timenow);
      t->t_lastbit = 0; // don't let the bitspacing averaging algorithm work on these bits
      if (t->lastdatabit == 0) { //TEMP this is bogus
         // the first new peak will be a data bit, when it comes
         t->clknext = false; }
      else { // the first new peak will be a clock bit, when it comes
         t->clknext = true;
         TRACE(clkwin, timenow, DNTICK, t); } } }


//*
