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
   if ((result->minbits == 3 && ntrks == 9 && data[0] == 0x26 && data[1] == 0 && data[2] == 0x26)  // 9 trk: trks 367, then nothing, then 367
         || (result->minbits == 2 && ntrks == 7 && data[0] == 0x1e && data[1] == 0x1e) // 7trk: trks 8421, then nothing, then 8421
      ) result->blktype = BS_TAPEMARK; // then it's the bizarre tapemark
   else {
      result->blktype = BS_BLOCK;
      result->vparity_errs = 0;
      int crc = 0;
      int lrc = 0;
      if (result->minbits > 2) {
         for (int i = 0; i < result->minbits - (ntrks == 7 ? 1 : 2); ++i) {  // count parity errors, and check the CRC/LRC at the end
            if (parity(data[i]) != expected_parity) ++result->vparity_errs;
            lrc ^= data[i];
            crc ^= data[i]; // C0..C7,P  (See IBM Form A22-6862-4)
            if (crc & 2) crc ^= 0xf0; // if P will become 1 after rotate, invert what will go into C2..C5
            int lsb = crc & 1; // rotate all 9 bits
            crc >>= 1;
            if (lsb) crc |= 0x100; }
         crc ^= 0x1af; // invert all except C2 and C4; note that the CRC could be zero if the number of data bytes is odd
         if (ntrks == 9) lrc ^= crc;  // LRC inlcudes the CRC (the manual doesn't say that!)
         result->crc = data[result->minbits - 2];
         result->lrc = data[result->minbits - 1];
         result->errcount = result->vparity_errs;
         if (ntrks == 9) { // only 9-track tapes have CRC
            if (crc != result->crc) {
               result->crc_bad = true;
               ++result->errcount; }
            dlog("crc is %03X, should be %03X\n", result->crc, crc); }
         if (lrc != result->lrc) {
            result->lrc_bad = true;
            ++result->errcount; }
         dlog("lrc is %03X, should be %03X\n", result->lrc, lrc);
         result->minbits -= ntrks == 9 ? 2 : 1;  // don't include CRC (if present) and LRC in the data
         result->maxbits -= ntrks == 9 ? 2 : 1; } }
   interblock_expiration = timenow + NRZI_IBG_SECS;  // ignore data for a while until we're well into the IBG
}

void nrzi_end_of_block(void) {
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   float avg_bit_spacing = 0;
   nrzi.datablock = false;
   result->minbits = MAXBLOCK;
   result->maxbits = 0;
   for (int trk = 0; trk < ntrks; ++trk) { //
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (float)(t->t_lastbit - t->t_firstbit) / t->datacount;
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount;
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain; }
   result->avg_bit_spacing = avg_bit_spacing / ntrks;
   dlog("NRZI end_of_block, min %d max %d, avgbitspacing %f uS at %.7lf tick %.1lf\n",
        result->minbits, result->maxbits, result->avg_bit_spacing*1e6, timenow, TICK(timenow));
   if (result->maxbits <= 1) {  // leave result-blktype == BS_NONE
      dlog("   ignoring noise block of length %d at %.7lf\n", result->maxbits, timenow); }
   else {
      if (result->minbits != result->maxbits) {  // different number of bits in different tracks
         if (DEBUG) show_track_datacounts("*** malformed block");
         result->blktype = BS_MALFORMED; }
      else nrzi_postprocess(); } }

void nrzi_addbit(struct trkstate_t *t, byte bit, double t_bit) { // add a NRZI bit
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   t->t_lastbit = t_bit;
   if (t->datacount == 0) {
      t->t_firstbit = t_bit; // record time of first bit in the datablock
      //assert(t->v_top > t->v_bot, "v_top < v_bot in nrzi_addbit at %.7lf", t_bit);
      t->max_agc_gain = t->agc_gain; }
#if 0 // we use the predefined value
   if (t->v_avg_height == 0 && bit == 1) { // make our first estimate of the peak-to-peak height
      t->v_avg_height = 2 * (max(abs(t->v_bot), abs(t->v_top))); // starting avg peak-to-peak is twice the first top or bottom
      if (t->trknum == TRACETRK)
         dlog("trk %d first avg height %.2f (v_top %.2f, v_bot %.2f) when adding %d at %.7lf\n", t->trknum, t->v_avg_height, t->v_top, t->v_bot, bit, t_bit); }
#endif
   if (!nrzi.datablock) { // this is the begining of data for this block
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg;
      dlog("trk %d starts the data blk at %.7lf tick %.1lf, agc=%f, clkavg=%.2f\n",
           t->trknum, t_bit, TICK(t_bit), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6);
      nrzi.datablock = true; }
   if (trace_on)
      dlog (" [add a %d to %d bytes on trk %d at %.7lf tick %.1lf, lastpeak %.7lf tick %.1lf; now: %.7lf tick %.1lf, bitspacing %.2f, agc %.2f]\n",
            bit, t->datacount, t->trknum, t_bit, TICK(t_bit),  t->t_lastpeak, TICK(t->t_lastpeak),
            timenow, TICK(timenow), nrzi.clkavg.t_bitspaceavg*1e6, t->agc_gain);
   uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
   data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
   data_time[t->datacount] = t_bit;
   if (t->datacount < MAXBLOCK) ++t->datacount;
   if (nrzi.post_counter > 0 && bit) { // we're at the end of a block and get a one: must be LRC or CRC
      nrzi.t_lastclock = t_bit - nrzi.clkavg.t_bitspaceavg; // reset when we thought the last clock was
   } }

void nrzi_deletebits(void) {
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *t = &trkstate[trknum];
      assert(t->datacount > 0, "bad NRZI data count");
      --t->datacount; } }

void nrzi_bot(struct trkstate_t *t) { // detected a bottom
   //if (trace_on) dlog("trk %d bot at %.7f tick %.1lf, agc %.2f\n",
   //                      t->trknum, t->t_bot, TICK(t->t_bot), t->agc_gain);
   if (PEAK_STATS && nrzi.t_lastclock != 0 && nrzi.datablock && nrzi.post_counter == 0)
      record_peakstat(nrzi.clkavg.t_bitspaceavg, (float)(t->t_bot - nrzi.t_lastclock), t->trknum);
   if (t->t_bot < nrzi.t_last_midbit && nrzi.post_counter == 0) {
      dlog("---trk %d bot of %.2fV at %.7lf tick %.1lf found at %.7lf tick %.1lf is %.2lfuS before midbit at %.7lf tick %.1f\n"
           "    lastclock %.7lf tick %.1f, AGC %.2f, bitspace %.2f, datacnt %d\n",
           t->trknum, t->v_bot, t->t_bot, TICK(t->t_bot), timenow, TICK(timenow), (nrzi.t_last_midbit - t->t_bot)*1e6,
           nrzi.t_last_midbit, TICK(nrzi.t_last_midbit), nrzi.t_lastclock, TICK(nrzi.t_lastclock), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6, t->datacount);
      ++block.results[block.parmset].missed_midbits; }
   nrzi_addbit(t, 1, t->t_bot);  // add a data 1
   t->hadbit = true;
   if (t->peakcount > AGC_ENDBASE && t->v_avg_height_count == 0) // if we're far enough into the data
      adjust_agc(t); }



void nrzi_top(struct trkstate_t *t) {  // detected a top
   //if (trace_on) dlog("trk %d top at %.7f tick %.1lf, agc %.2f\n",
   //                      t->trknum, t->t_top, TICK(t->t_top), t->agc_gain);
   if (PEAK_STATS && nrzi.t_lastclock != 0 && nrzi.datablock && nrzi.post_counter == 0)
      record_peakstat(nrzi.clkavg.t_bitspaceavg, (float)(t->t_top-nrzi.t_lastclock), t->trknum);
   if (t->t_top < nrzi.t_last_midbit && nrzi.post_counter == 0) {
      dlog("---trk %d top of %.2fV at %.7lf tick %.1lf found at %.7lf tick %.1lf is %.2lfuS before midbit at %.7lf tick %.1f\n"
           "    lastclock %.7lf tick %.1f, AGC %.2f, bitspace %.2f, datacnt %d\n",
           t->trknum, t->v_top, t->t_top, TICK(t->t_top), timenow, TICK(timenow), (nrzi.t_last_midbit - t->t_top)*1e6,
           nrzi.t_last_midbit, TICK(nrzi.t_last_midbit), nrzi.t_lastclock, TICK(nrzi.t_lastclock), t->agc_gain, nrzi.clkavg.t_bitspaceavg*1e6, t->datacount);
      ++block.results[block.parmset].missed_midbits; }
   nrzi_addbit(t, 1, t->t_top); // add a data 1
   t->hadbit = true;
   if (t->v_top <= t->v_bot) dlog("!!! top trk %d vtop %.2f less than vbot %.2f at %.7lf tick %.1lf\n", t->trknum, t->v_top, t->v_bot, timenow, TICK(timenow));
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
         if (TRACING)
            dlog("trk %d avg peak-to-peak after %d transitions is %.2fV at %.7lf\n",  t->trknum, AGC_ENDBASE-AGC_STARTBASE, t->v_avg_height, timenow);
         assert(t->v_avg_height>0, "avg peak-to-peak voltage isn't positive");
         t->v_avg_height_count = 0; }
      else adjust_agc(t); // otherwise adjust AGC
   } }

void nrzi_midbit(void) { // we're in between NRZI bit times: make decisions about the previous bits
   int numbits = 0;
   double avg_pos = 0;
   TRACE(midbit, timenow, UPTICK, NULLP);
   nrzi.t_last_midbit = timenow;
   if (trace_on) dlog("midbit %d at %.7lf tick %.1lf\n", nrzi.post_counter, timenow, TICK(timenow));
   if (nrzi.post_counter == 0 // if we're not at the end of the block yet
         || (nrzi.post_counter == 4 && ntrks == 9) // or we're 1.5 bit times past the CRC for 9-track tapes
         || nrzi.post_counter == 8 // or we're 1.5 bit times past the LRC for 7- or 9-track tapes
      ) {  // then process this interval
      for (int trknum = 0; trknum < ntrks; ++trknum) {
         struct trkstate_t *t = &trkstate[trknum];
         if (!t->hadbit) { // if this track had no transition at the last clock
            nrzi_addbit(t, 0, timenow - nrzi.clkavg.t_bitspaceavg * NRZI_MIDPOINT); // add a zero bit at approximately the last clock time
         }
         else { // there was a transition on this track: accumulate the average position of the clock time
            //if (trace_on) dlog( " %d:%.1lf ", t->trknum, TICK(t->t_lastpeak));
            avg_pos += t->t_lastpeak;
            t->hadbit = false;
            ++numbits; } } // for all tracks

      if (numbits > 0) { // at least one track had a flux transition at the last clock
         avg_pos /= numbits;  // the average real position
         TRACE(avgpos, avg_pos, UPTICK, NULLP);
         double expected_pos, adjusted_pos;
         expected_pos = nrzi.t_lastclock + nrzi.clkavg.t_bitspaceavg;  // where we expected the position to be
         if (!nrzi.datablock || nrzi.post_counter > 0)
            adjusted_pos = avg_pos; // don't adjust from actual position at the beginning or in CRC/LRC territory
         else adjusted_pos = expected_pos + PARM.pulse_adj * (avg_pos - expected_pos); // adjust some amount away from the expected position
         float delta = (float)(adjusted_pos - nrzi.t_lastclock);
         if (nrzi.post_counter == 0) {
            float oldavg;
            if (DEBUG) oldavg = nrzi.clkavg.t_bitspaceavg;
            adjust_clock(&nrzi.clkavg, delta, 0);  // adjust the clock rate based on the average position
            if (DEBUG && trace_on)
               dlog("adjust clk at %.7lf tick %.1lf with delta %.2fus into avg %.2fus making %.2fus, avg pos %.7lf tick %.1lf, adj pos %.7lf tick %.1lf\n", //
                    timenow, TICK(timenow), delta*1e6, oldavg*1e6, nrzi.clkavg.t_bitspaceavg*1e6,
                    avg_pos, TICK(avg_pos), adjusted_pos, TICK(adjusted_pos)); //
         }
         nrzi.t_lastclock = adjusted_pos;  // use it as the last clock position
         if (nrzi.post_counter) ++nrzi.post_counter; // we in the post-block: must have been CRC or LRC
      }

      else { // there were no transitions (for while), so we must be at the end of the block
         // The CRC is supposed to be after 3 quiet bit times (post_counter == 3), and then
         // the LRC is supposed to be after another 3 quiet bit times (post counter == 7).
         // The problem is that they seem often to be delayed by about half a bit, which screws up our clocking. So we:
         //  1. don't update the clock speed average during this post-data time.
         //  2. Estimate the last clock time based on any CRC or LRC 1-bit transitions we see.
         //  3. Wait until one bit time later to accmulate the 0-bits for the CRC/LRC, ie post_counter 4 and 8.
         dlog("start postcounter at %.7lf tick %.1lf\n", timenow, TICK(timenow));
         nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // compute a hypothetical last clock position
         if (nrzi.post_counter == 0) nrzi_deletebits(); // delete all the zero bits we just added
         ++nrzi.post_counter; } }

   else if (nrzi.post_counter) {
      ++nrzi.post_counter; // we're not processing this interval; just count it
      nrzi.t_lastclock += nrzi.clkavg.t_bitspaceavg; // and advance the hypothetical last clock position
   }

   if (nrzi.post_counter > 8) {
      nrzi_end_of_block(); }
   if (trace_on) dlog("midbit at %.7lf tick %.1lf, %d transitions, lastclock %.7lf tick %.1lf, trk0 %d bytes, post_ctr %d\n", //
                         timenow, TICK(timenow), numbits, nrzi.t_lastclock, TICK(nrzi.t_lastclock),
                         trkstate[0].datacount, nrzi.post_counter); //
}

//*
