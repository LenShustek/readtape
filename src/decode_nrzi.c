//file: decode_nrzi.c
/******************************************************************************

   decode routines specific to:
      7-track 200, 556, and 800 BPI tape formats
      9-track 800 BPI tape format

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
   Well-formed block processing routines for 7-track or 9-track NRZI
******************************************************************************************************************************/
void nrzi_postprocess(void) {
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   //dumpdata(data, result->minbits);
   result->blktype = BS_BLOCK;
   result->vparity_errs = 0;
   if (result->minbits > 8) {
      /* The bytes at the end are as follows:
             minbits- 9   8   7   6     5    4      3   2   1
         for 9-track: xx  00  00  crc?  crc  crc?   00  00  LRC
         for 7-track: xx  00  00  lrc?  lrc  lrc?   00  00  00
      The xxx? entries are early and late positions that violate spec but which we've seen.
      */
      if (ntrks == 9) {
         result->crc = data[result->minbits - 6] | data[result->minbits - 5] | data[result->minbits - 4];
         result->lrc = data[result->minbits - 1]; }
      else if (ntrks == 7) {
         result->lrc = data[result->minbits - 6] | data[result->minbits - 5] | data[result->minbits - 4]; }
      result->maxbits -= 8;  // now remove those ending bytes
      result->minbits -= 8;
      set_expected_parity(result->maxbits);
      int crc = 0, lrc = 0;
      for (int i = 0; i < result->minbits; ++i) {  // count parity errors, and compute the CRC/LRC
         if (parity(data[i]) != expected_parity) {
            dlog("parity err in nrzi_postprocess() at index %d data %03X time %.8lf tick %.1lf\n",
                 i, data[i], data_time[i], TICK(data_time[i]));
            ++result->vparity_errs; }
         lrc ^= data[i];
         crc ^= data[i]; // C0..C7,P  (See IBM Form A22-6862-4)
         if (crc & 2) crc ^= 0xf0; // if P will become 1 after rotate, invert what will go into C2..C5
         int lsb = crc & 1; // rotate all 9 bits
         crc >>= 1;
         if (lsb) crc |= 0x100; }
      crc ^= 0x1af; // invert all except C2 and C4; note that the CRC could be zero if the number of data bytes is odd
      if (ntrks == 9) { // only 9-track tapes have CRC
         lrc ^= crc;  // LRC inlcudes the CRC (the manual doesn't say that!)
         if (crc != result->crc) {
            ++result->crc_errs;
            dlog("crc is %03X, should be %03X\n", result->crc, crc); } }
      if (lrc != result->lrc) {
         ++result->lrc_errs;
         dlog("lrc is %03X, should be %03X\n", result->lrc, lrc); } } }

void nrzi_end_of_block(void) {
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   if (block.endblock_done) return;
   block.endblock_done = true;

   float avg_bit_spacing = 0;
   nrzi.datablock = false;
   result->minbits = MAXBLOCK;
   result->maxbits = 0;
   for (int trk = 0; trk < ntrks; ++trk) { //
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (float)(t->t_lastbit - t->t_firstbit) / t->datacount;
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount;
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain;
      if (result->alltrk_min_agc_gain > t->min_agc_gain) result->alltrk_min_agc_gain = t->min_agc_gain; }
   result->avg_bit_spacing = avg_bit_spacing / ntrks;
   dlog("NRZI end of block %d, min %d max %d, avgbitspacing %f uS at %.8lf tick %.1lf\n",
        numblks+1, result->minbits, result->maxbits, result->avg_bit_spacing*1e6, timenow, TICK(timenow));
   // see what we think this is, based on the length
   if ( // maybe it's a tapemark, which is pretty bizarre
      result->minbits == 9 // the initial 1-bit plus 8 bits of post-block
      && (ntrks == 9 && data[0] == 0x26 && data[8] == 0x26  // 9 trk: trks 367, then 7 bits of zeros, then 367
          ||  ntrks == 7 && data[0] == 0x1e && (data[3] == 0x1e || data[4] == 0x1e))) { // 7trk: trks 8421, then 2 or 3 bits of zeros, then 8421
      result->blktype = BS_TAPEMARK; }
   else if (result->maxbits <= NRZI_MIN_BLOCK) {  // too small, but not tapemark: just noise
      dlog("   detected noise block of length %d at %.8lf\n", result->maxbits, timenow);
      result->blktype = BS_NOISE; }
   else if (result->maxbits - result->minbits > NRZI_MAX_MISMATCH) {  // very different number of bits in different tracks
      if (verbose_level & VL_TRACKLENGTHS) show_track_datacounts("*** trkmismatched block");
      result->blktype = BS_BADBLOCK;
      result->track_mismatch = result->maxbits - result->minbits; }
   else { // finally, perhaps a good block
      nrzi_postprocess(); }
   num_trks_idle = ntrks;  // declare all tracks idle
   interblock_counter = (int)(NRZI_IBG_SECS / sample_deltat);  // ignore data for a while until we're well into the IBG
}

#if CORRECT
void nrzi_correct_error(int last_complete_byte) {
   // if the latest byte has a parity error, and one track has a much higher
   // gain than all the others, change its data to make the parity correct
   float highest = 0, next_highest = 0;
   int badtrk = -1;
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      float gain = trkstate[trknum].agc_gain;
      if (gain > highest) {
         next_highest = highest;
         highest = gain; badtrk = trknum; }
      else if (gain > next_highest) next_highest = gain; }
   assert(badtrk >= 0, "nrzi_corrrect_error pb");
   //dlog("trying to correct error at byte %d with high AGC %.2f, next highest %.2f: ",
   //   last_complete_byte, highest, next_highest);
   if (highest >= NRZI_BADTRK_FACTOR * next_highest) {
      uint16_t mask = 1 << (ntrks - 1 - badtrk);  // change this track's bit in the data array
      data[last_complete_byte] ^= mask;
      data_faked[last_complete_byte] |= mask;
      ++block.results[block.parmset].corrected_bits;
      block.results[block.parmset].faked_tracks |= mask;
      dlog("corrected track %d at byte %d because its AGC is %.2f and the next highest is %.2f, at time %.8lf tick %.1lf\n",
           badtrk, last_complete_byte, highest, next_highest, timenow, TICK(timenow)); //
   }
   //else dlog("not corrected\n");
}
#endif

void nrzi_addbit(struct trkstate_t *t, byte bit, double t_bit) { // add a NRZI bit
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   t->t_lastbit = t_bit;
   if (t->datacount == 0) {
      t->t_firstbit = t_bit; // record time of first bit in the datablock
      //assert(t->v_top > t->v_bot, "v_top < v_bot in nrzi_addbit at %.8lf", t_bit);
      t->max_agc_gain = t->agc_gain; }
#if 0 // we use the predefined value
   if (t->v_avg_height == 0 && bit == 1) { // make our first estimate of the peak-to-peak height
      t->v_avg_height = 2 * (max(abs(t->v_bot), abs(t->v_top))); // starting avg peak-to-peak is twice the first top or bottom
      if (t->trknum == TRACETRK)
         dlog("trk %d first avg height %.2f (v_top %.2f, v_bot %.2f) when adding %d at %.8lf\n", t->trknum, t->v_avg_height, t->v_top, t->v_bot, bit, t_bit); }
#endif
   if (!nrzi.datablock) { // this is the begining of data for this block
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg; // fake the previous bit timing
      nrzi.t_last_midbit = nrzi.t_lastclock + PARM.midbit*nrzi.clkavg.t_bitspaceavg;
      dlog("trk %d starts the data blk at %.8lf tick %.1lf, agc=%f, clkavg=%.2f\n",
           t->trknum, t_bit, TICK(t_bit), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6);
      block.t_blockstart = timenow;
      nrzi.datablock = true; }
   if (trace_on)
      dlog (" [add a %d to %d bytes on trk %d at %.8lf tick %.1lf, lastpeak %.8lf tick %.1lf; now: %.8lf tick %.1lf, bitspacing %.2f, agc %.2f]\n",
            bit, t->datacount, t->trknum, t_bit, TICK(t_bit),  t->t_lastpeak, TICK(t->t_lastpeak),
            timenow, TICK(timenow), nrzi.clkavg.t_bitspaceavg*1e6, t->agc_gain);
   uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
   data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
   data_time[t->datacount] = t_bit;
   if (t->datacount < MAXBLOCK) ++t->datacount;
   if (nrzi.post_counter > 0 && bit) { // we're at the end of a block and get a one: must be LRC or CRC
      // the clock has been free-running, so maybe use this one bit to realign it
      if (nrzi.t_lastclock < t_bit - (2-PARM.midbit)*nrzi.clkavg.t_bitspaceavg) // we haven't yet processed the earlier window
         nrzi.t_lastclock = t_bit - 2 * nrzi.clkavg.t_bitspaceavg; // tweak when we thought the clock before last was
   } }

void nrzi_deletebits(int howmany) {
   dlog("deleting %d bits at postcounter %d\n", howmany, nrzi.post_counter);
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      assert(t->datacount >= howmany, "bad NRZI data count");
      t->datacount -= howmany; } }

void nrzi_bot(struct trkstate_t *t) { // detected a bottom
   //if (trace_on) dlog("trk %d bot at %.7f tick %.1lf, agc %.2f\n",
   //                      t->trknum, t->t_bot, TICK(t->t_bot), t->agc_gain);
   if (PEAK_STATS && nrzi.t_lastclock != 0 && nrzi.datablock && nrzi.post_counter == 0)
      record_peakstat(nrzi.clkavg.t_bitspaceavg, (float)(t->t_bot - nrzi.t_lastclock), t->trknum);
   if (t->t_bot < nrzi.t_last_midbit && nrzi.post_counter == 0) {
      dlog("---trk %d bot of %.2fV at %.8lf tick %.1lf found at %.8lf tick %.1lf is %.2lfuS before midbit at %.8lf tick %.1f\n"
           "    lastclock %.8lf tick %.1f, AGC %.2f, bitspace %.2f, datacnt %d\n",
           t->trknum, t->v_bot, t->t_bot, TICK(t->t_bot), timenow, TICK(timenow), (nrzi.t_last_midbit - t->t_bot)*1e6,
           nrzi.t_last_midbit, TICK(nrzi.t_last_midbit), nrzi.t_lastclock, TICK(nrzi.t_lastclock), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6, t->datacount);
      ++block.results[block.parmset].missed_midbits; }
   nrzi_addbit(t, 1, t->t_bot);  // add a data 1
   if (t->peakcount > AGC_ENDBASE && t->v_avg_height_count == 0) // if we're far enough into the data
      adjust_agc(t); }

void nrzi_top(struct trkstate_t *t) {  // detected a top
   //if (trace_on) dlog("trk %d top at %.7f tick %.1lf, agc %.2f\n",
   //                      t->trknum, t->t_top, TICK(t->t_top), t->agc_gain);
   if (PEAK_STATS && nrzi.t_lastclock != 0 && nrzi.datablock && nrzi.post_counter == 0)
      record_peakstat(nrzi.clkavg.t_bitspaceavg, (float)(t->t_top - nrzi.t_lastclock), t->trknum);
   if (t->t_top < nrzi.t_last_midbit && nrzi.post_counter == 0) {
      dlog("---trk %d top of %.2fV at %.8lf tick %.1lf found at %.8lf tick %.1lf is %.2lfuS before midbit at %.8lf tick %.1f\n"
           "    lastclock %.8lf tick %.1f, AGC %.2f, bitspace %.2f, datacnt %d\n",
           t->trknum, t->v_top, t->t_top, TICK(t->t_top), timenow, TICK(timenow), (nrzi.t_last_midbit - t->t_top)*1e6,
           nrzi.t_last_midbit, TICK(nrzi.t_last_midbit), nrzi.t_lastclock, TICK(nrzi.t_lastclock), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6, t->datacount);
      ++block.results[block.parmset].missed_midbits; }
   nrzi_addbit(t, 1, t->t_top); // add a data 1
   if (t->v_top <= t->v_bot) dlog("!!! top trk %d vtop %.2f less than vbot %.2f at %.8lf tick %.1lf\n", t->trknum, t->v_top, t->v_bot, timenow, TICK(timenow));
   if (NRZI_RESET_SPEED && !nrzi.reset_speed && t->datacount == 2) { // reset speed based on the first low-to-high transition we see
      nrzi.clkavg.t_bitspaceavg = (float)(t->t_top - t->t_bot);
      for (int i = 0; i < CLKRATE_WINDOW; ++i) nrzi.clkavg.t_bitspacing[i] = nrzi.clkavg.t_bitspaceavg;
      ips = 1 / (nrzi.clkavg.t_bitspaceavg * bpi);
      nrzi.reset_speed = true;
      dlog("reset speed by trk %d to %f IPS\n", t->trknum, ips); }
   if (t->peakcount >= AGC_STARTBASE && t->peakcount <= AGC_ENDBASE) { // accumulate initial baseline peak-to-peak voltages
      t->v_avg_height_sum += t->v_top - t->v_bot;
      ++t->v_avg_height_count;
      t->v_heights[t->heightndx] = t->v_top - t->v_bot;
      if (++t->heightndx >= PARM.agc_window) t->heightndx = 0; }
   else if (t->peakcount > AGC_ENDBASE) { // we're beyond the first set of peaks and have some peak-to-peak history
      if (t->v_avg_height_count) { // if the is the first time we've gone beyond
         t->v_avg_height = t->v_avg_height_sum / t->v_avg_height_count; // then compute avg peak-to-peak voltage
         dlogtrk("trk %d avg peak-to-peak after %d transitions is %.2fV at %.8lf\n",  t->trknum, AGC_ENDBASE-AGC_STARTBASE, t->v_avg_height, timenow);
         assert(t->v_avg_height>0, "avg peak-to-peak voltage isn't positive");
         t->v_avg_height_count = 0; }
      else adjust_agc(t); // otherwise adjust AGC
   } }

void nrzi_zerocheck(void) {  // we're more or less in the vicinity of the next clock boundary
   // - check for missing peaks near the previous clock, which represent zero bits
   // - delete extra data we added if there were multiple peaks near the previous clock (7/18/2022)
   // - adjust the clock edge position and rate based on the average location of all one-bit transitions we see there
   // - check for entering the end-of-block area when there is silence on all tracks
   int numbits = 0, numlaterbits = 0;
   TRACE(zerchk, timenow, UPTICK, NULLP);
   double left_edge = nrzi.t_last_midbit;  // the left edge of the window for a peak
   double right_edge = nrzi.t_lastclock + (1 + PARM.midbit) * nrzi.clkavg.t_bitspaceavg; // the right edge
   nrzi.t_last_midbit = right_edge; // the new last midbit position
   if (DEBUG && trace_on) dlog("zerocheck: postctr %d, left %.8lf tick %.1lf, right %.8lf tick %.1lf, at %.8lf tick %.1lf\n",
                                  nrzi.post_counter, left_edge, TICK(left_edge), right_edge, TICK(right_edge), timenow, TICK(timenow));
   double avg_pos = 0;
   int last_complete_byte = 0; // only for debugging
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      bool lastpeak_in_window = t->t_lastpeak > left_edge && t->t_lastpeak < right_edge;
      bool prevlastpeak_in_window = t->t_prevlastpeak > left_edge && t->t_prevlastpeak < right_edge;
      if (lastpeak_in_window) {
         avg_pos += t->t_lastpeak; // the last peak was in the subject window
         ++numbits;
         if (prevlastpeak_in_window) { // If the previous peak was also in the window, we have a noisy peak.
            --t->datacount;  // delete the 1-bit for one of the peaks
            if (DEBUG && trace_on)
               dlog("   trk %d deleted 1-bit from noise peak at %.8lf tick %.1lf\n",
                    trknum, t->t_lastpeak, TICK(t->t_lastpeak)); }
         if (DEBUG) last_complete_byte = t->datacount - 1; }
      else if (prevlastpeak_in_window) {
         avg_pos += t->t_prevlastpeak; // the peak before that was in the window
         ++numbits;
         if (DEBUG) last_complete_byte = t->datacount - 2; }
      else { // neither: we missed a peak on this track and must record a zero bit
         if (t->t_lastpeak > right_edge) { // but if there was a subsequent peak,
            --t->datacount; // temporarily erase that one bit
            nrzi_addbit(t, 0, nrzi.t_lastclock + nrzi.clkavg.t_bitspaceavg); // add the zero bit
            nrzi_addbit(t, 1, t->t_lastpeak); // and then put back the one bit
            ++numlaterbits; }
         else  nrzi_addbit(t, 0, nrzi.t_lastclock + nrzi.clkavg.t_bitspaceavg); // otherwise just add the zero
      } } // for all tracks
   if (numbits > 0) { // at least one track had a flux transition at the last clock
      if (nrzi.post_counter == 1) { // if we had just barely started the end of block
         nrzi.post_counter = 0; // cancel it; the all zeroes must be from be a bit error in this block
         dlog("cancelling postcounter at %.8lf tick %.1lf\n", timenow, TICK(timenow)); }
      avg_pos /= numbits;  // compute the average of the peak locations
      TRACE(avgpos, avg_pos, UPTICK, NULLP);
      double expected_pos, adjusted_pos;
      expected_pos = nrzi.t_lastclock + nrzi.clkavg.t_bitspaceavg;  // where we expected the position to be
      if (!nrzi.datablock || nrzi.post_counter > 0)
         adjusted_pos = avg_pos; // (don't adjust from actual position at the beginning or in CRC/LRC territory)
      else adjusted_pos = expected_pos + PARM.pulse_adj * (avg_pos - expected_pos); // adjust some amount away from the expected position
      float delta = (float)(adjusted_pos - nrzi.t_lastclock);
      if (nrzi.post_counter == 0) { // adjust the clock rate, perhaps with some low-pass filtering
         float oldavg;
         if (DEBUG) oldavg = nrzi.clkavg.t_bitspaceavg;
         adjust_clock(&nrzi.clkavg, delta, 0);  // adjust the clock rate based on the average position
         if (DEBUG && trace_on)
            dlog("adjust clk at %.8lf tick %.1lf with delta %.2fus into avg %.2fus making %.2fus, avg pos %.8lf tick %.1lf, adj pos %.8lf tick %.1lf\n", //
                 timenow, TICK(timenow), delta*1e6, oldavg*1e6, nrzi.clkavg.t_bitspaceavg*1e6,
                 avg_pos, TICK(avg_pos), adjusted_pos, TICK(adjusted_pos)); //
      }
      nrzi.t_lastclock = adjusted_pos;  // use it as the last clock position
      if (DEBUG && trace_on && parity(data[last_complete_byte]) != expected_parity) TRACE(parerr, timenow, UPTICK, NULLP);
      if (DEBUG && trace_on) {
         dlog(" byte %d is %03X, parity %s\n", last_complete_byte, data[last_complete_byte],
              parity(data[last_complete_byte]) == expected_parity ? "ok" : "bad"); }
      //if (DEBUG && !doing_deskew && nrzi.post_counter == 0 && parity(data[last_complete_byte]) != expected_parity) {
      //   dlog("parity err at datacount %d at %.8lf; AGC ", last_complete_byte, timenow);
      //   for (int i = 0; i < ntrks; ++i) dlog("%d: %.1f, ", i, trkstate[i].agc_gain);
      //   dlog("\n"); }
#if CORRECT
      if (do_correction && parity(data[last_complete_byte]) != expected_parity) nrzi_correct_error(last_complete_byte);
#endif
      if (nrzi.post_counter) ++nrzi.post_counter; //in the post-block: move to the next phase
   }
   else { // no transitions on any track
      if (numlaterbits == 0  // if no later bits coming
            && nrzi.post_counter == 0) { // and we're not already doing the end of block
         dlog("start postcounter, trk 0 count %d at %.8lf tick %.1lf\n", trkstate[0].datacount, timenow, TICK(timenow));
         nrzi.post_counter = 1; }
      else if (nrzi.post_counter) ++nrzi.post_counter; //already in the post-block: move to the next phase
      nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // compute a hypothetical last clock position
   }
   if (nrzi.post_counter >= 8) nrzi_end_of_block(); }

//*
