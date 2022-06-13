//file: decode_ww.c
/******************************************************************************

decode routines specific to Whirlwind I 6-track 100 BPI tapes

---> See readtape.c for the merged change log.

*******************************************************************************
Copyright (C) 2019, Len Shustek

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

// We can't completely restart the block decoding state at the end of a block
// because the blocks can be only 1 bit apart and we need to keep the
// information about peaks in progress on all the tracks.

void ww_init_blockstate(void) { // what we can initialize before a new block
   memset(&block.results[block.parmset], 0, sizeof(struct results_t));
   block.results[block.parmset].blktype = BS_NONE;
   block.results[block.parmset].alltrk_max_agc_gain = 0.0;
   block.results[block.parmset].alltrk_min_agc_gain = FLT_MAX;
   for (int trknum = 0; trknum < ntrks; ++trknum) {
      struct trkstate_t *trk = &trkstate[trknum];
      trk->max_agc_gain = 0.0;
      trk->min_agc_gain = FLT_MAX;
      trk->t_lastpeak = trk->t_prevlastpeak = 0; } // make sure adjust_clock() below will work ok
   // can't zero out entire ww. state structure because we need things like blockmark_queued, etc.
   init_clkavg(&ww.clkavg, 1 / (bpi*ips));
   ww.t_lastclkpulsestart = ww.t_lastclkpulseend = ww.t_lastpriclkpulseend = 0;
   ww.datablock = false;
   ww.datacount = 0;
   data[0] = 0; // needs to start at zero because we record only one bits, but not zero bits
}

int ww_chk_databit(double clkendtime, enum wwtrk_t type, uint16_t bitmask) {
   // check for a data pulse start on this track in one bit time before this clock end
   // return 0 if we don't have this track, 1 for a 1-bit, and 2 for a 0-bit
   // that means OR of the results from the primary and alternate tracks is 3 if they are both present and differ
   int trk = ww_type_to_trk[type];
   if (trk < 0) return 0; // we don't have this track
   assert(trk < ntrks, "bad trk in ww_chk_databit: %d", trk);
   struct trkstate_t *t = &trkstate[trk];
   if (t->t_lastpulsestart > clkendtime - ww.clkavg.t_bitspaceavg && t->t_lastpulsestart < clkendtime) {
      // if there was a pulse end between the last two clock pulse starts,
      // record a 1 for this track type during this clock interval
      if (0 && !doing_deskew) rlog("  add 1 trk %d mask %X datacount %d, bit time "TIMEFMT", clockpulseend %.7f tick %.1lf\n",
                                      trk, bitmask, ww.datacount, TIMETICK(t->t_lastpulsestart), TIMETICK(clkendtime));
      data[ww.datacount] |= bitmask;
      return 1; }
   return 2; }

void ww_chk_databits(double clkendtime) { // check for data pulse starts that occurred between clock pulse ends
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   bool got_priMSB, got_priLSB;
   if (0 && !doing_deskew) rlog("chk bits, datacount %d, clockpulseend "TIMEFMT", timenow "TIMEFMT"\n",
                                   ww.datacount, TIMETICK(clkendtime), TIMETICK(timenow));
   if (((got_priMSB = ww_chk_databit(clkendtime, WWTRK_PRIMSB, 0x02)) | ww_chk_databit(clkendtime, WWTRK_ALTMSB, 0x02)) == 3) {
      ++result->ww_missing_onebit;   // if both MSB tracks are there and they don't agree, flag a warning
      if (verbose_level & VL_WARNING_DETAIL && !doing_deskew) {
         if (!got_priMSB)
            rlog("  missing primary MSB   at "TIMEFMT", last pri pulse end "TIMEFMT", bitspacing %.1f\n",
                 TIMETICK(clkendtime), TIMETICK(trkstate[ww_type_to_trk[WWTRK_PRIMSB]].t_lastpulseend), ww.clkavg.t_bitspaceavg * 1e6);
         else  rlog("  missing alternate MSB at "TIMEFMT", last alt pulse end "TIMEFMT", bitspacing %.1f\n",
                       TIMETICK(clkendtime), TIMETICK(trkstate[ww_type_to_trk[WWTRK_ALTMSB]].t_lastpulseend), ww.clkavg.t_bitspaceavg * 1e6); } }
   if (((got_priLSB = ww_chk_databit(clkendtime, WWTRK_PRILSB, 0x01)) | ww_chk_databit(clkendtime, WWTRK_ALTLSB, 0x01)) ==3) {
      ++result->ww_missing_onebit;  // if both LSB tracks are there and they don't agree, flag a warning
      if (verbose_level & VL_WARNING_DETAIL && !doing_deskew) {
         if (!got_priLSB)
            rlog("  missing primary LSB   at "TIMEFMT", last pri pulse end "TIMEFMT", bitspacing %.1f\n",
                 TIMETICK(clkendtime), TIMETICK(trkstate[ww_type_to_trk[WWTRK_PRILSB]].t_lastpulseend), ww.clkavg.t_bitspaceavg * 1e6);
         else  rlog("  missing alternate LSB at "TIMEFMT", last alt pulse end "TIMEFMT", bitspacing %.1f\n",
                       TIMETICK(clkendtime), TIMETICK(trkstate[ww_type_to_trk[WWTRK_ALTLSB]].t_lastpulseend), ww.clkavg.t_bitspaceavg * 1e6); } }
   if (trace_on) dlog("  ww data %3d: %d %d\n", ww.datacount, data[ww.datacount] >> 1, data[ww.datacount] & 1);
   TRACE(data, clkendtime, UPTICK + UPTICK*(data[ww.datacount]&0x03), &trkstate[0]); // create a 4-position line graph of the 2 bits on track 0
   data[++ww.datacount] = 0; // get ready for the next pair of bits
}

void ww_assemble_data(void) { // assemble the array of 2-bit characters into bytes
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   uint16_t temp_data[MAXBLOCK + 1];
   int outndx = 0, nibble_counter = 0;
   uint16_t accum;
   //rlog("assemble data, ww.datacount=%d\n", ww.datacount); //TEMP
   // special hack: if there is one more clock than a multiple of 8, then assume the first clock is noise
   // and discard the first two bits we derived from it.
   if (ww.datacount % 8 == 1 && ww.datacount >= 9) {
      for (int ndx = 0; ndx < ww.datacount - 1; ++ndx)
         data[ndx] = data[ndx + 1];
      --ww.datacount;
      result->ww_leading_clock = 1; }
   if (reverse_tape) { // assemble going backwards, into a temp array
      for (int inndx = ww.datacount - 1; inndx >= 0; --inndx) {
         //accum = (accum >> 2) | ((data[inndx] & 0x03) << 6); // shift in 2 more bits, least significant first
         accum = (accum << 2) | (data[inndx] & 0x03);   // shift in 2 more bits, most significant first
         if (++nibble_counter % 4 == 0) { // dump a full byte
            temp_data[outndx++] = (accum & 0xff) << 1; } } // create dummy parity bit on the right side
      for (int inndx = 0; inndx < outndx; ++inndx) // copy the temp array into the final data result
         data[inndx] = temp_data[inndx]; }
   else // assemble in place going forward, since we're making it smaller by 4x
      for (int inndx = 0; inndx < ww.datacount; ++inndx) {
         accum = (accum << 2) | (data[inndx] & 0x03);   // shift in 2 more bits, most significant first
         if (++nibble_counter % 4 == 0) { // dump a full byte
            data[outndx++] = (accum & 0xff) << 1; } } // create dummy parity bit on the right side
   result->minbits = result->maxbits = outndx;
   //for (int i = 0; i < result->minbits; ++i) rlog(" final data %2d: %04X\n", i, data[i]);
   if (ww.datacount % 8 != 0) { // should be a multiple of 16 bits, or 8 2-bit characters
      ++result->ww_bad_length;
      if (!doing_deskew && ww.datacount > 8 ) rlog("  *** the datacount for the next block is %d 2-bit characters, which is %d more than a multiple of 8\n",
               ww.datacount, ww.datacount % 8); }
   float target_bitspace = 1 / (bpi*ips);
   if (fabs(ww.clkavg.t_bitspaceavg - target_bitspace) / target_bitspace > WW_MAX_CLK_VARIATION) ++result->ww_speed_err; }

void ww_end_of_block(void) {
   set_expected_parity(0);
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   if (!doing_deskew) dlog("end of block at "TIMEFMT", datalength %d, lastclkpulseend "TIMEFMT" bitspaceavg %.1f\n",
                              TIMETICK(timenow), ww.datacount, TIMETICK(ww.t_lastclkpulseend), ww.clkavg.t_bitspaceavg*1e6);
   if (0 && DEBUG) for (int i = 0; i < ww.datacount; ++i)
         rlog("  data %3d: %d %d\n", i, data[i] >> 1, data[i] & 1);
   ww_assemble_data();  // post-process the 2-bit characters into bytes
   //ww_init_blockstate(); ...moved to before readblock() call in readtape.c
   result->blktype = BS_BLOCK;
   result->avg_bit_spacing = ww.clkavg.t_bitspaceavg;
   for (int trk = 0; trk < ntrks; ++trk) {  // compute max and min AGC gain for all tracks
      struct trkstate_t *t = &trkstate[trk];
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain;
      if (result->alltrk_min_agc_gain > t->min_agc_gain) result->alltrk_min_agc_gain = t->min_agc_gain; }
   // Before we return, check that there wasn't a pulse in a LSB channel while we
   // were in the process of detecting that the clock has stopped. If so, it's a blockmark
   // that we will need to return immediately the next time we are called for a block.
   // But we might see it on only one of the LSB tracks, so also record when it happens so that a
   // single pulse happening soon on the other LSB track doesn't generate another block mark.
   struct trkstate_t *t;
   t = &trkstate[ww_type_to_trk[WWTRK_PRILSB]];
   if (t->t_lastpulseend - ww.t_lastclkpulseend > ww.clkavg.t_bitspaceavg * WW_PEAKSCLOSE_BITS) {
      ww.blockmark_queued = true;
      ww.t_lastblockmark = t->t_lastpulseend; }
   t = &trkstate[ww_type_to_trk[WWTRK_ALTLSB]];
   if (t->t_lastpulseend - ww.t_lastclkpulseend > ww.clkavg.t_bitspaceavg * WW_PEAKSCLOSE_BITS) {
      ww.blockmark_queued = true;
      ww.t_lastblockmark = t->t_lastpulseend; }
   if (ww.blockmark_queued) {
      dlog("blockmark at "TIMEFMT" queued at "TIMEFMT"\n", TIMETICK(ww.t_lastblockmark), TIMETICK(timenow)); } }

void ww_blockmark(void) { // detected a block mark: a bit in the odd channel and nowhere else
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   //rlog("blockmark at %.8lf\n", timenow);
   result->blktype = BS_TAPEMARK;
   ww.blockmark_queued = false; }

void ww_pulse_start(struct trkstate_t *t, double t_pulse_start) { // detected the first half of a flux change pulse
   enum wwtrk_t wwtype = ww_trk_to_type[t->trknum];
   dlogtrk("trk %d type %c, ww_pulse_start at "TIMEFMT", lastpeak "TIMEFMT", prevlastpeak "TIMEFMT", bitspaceavg %.1f\n",
           t->trknum, WWTRKTYPE_SYMBOLS[wwtype], TIMETICK(t_pulse_start), TIMETICK(t->t_lastpeak), TIMETICK(t->t_prevlastpeak), ww.clkavg.t_bitspaceavg*1e6f);
   adjust_agc(t);
   t->t_lastpulsestart = t_pulse_start;
   if (wwtype == WWTRK_PRICLK || wwtype == WWTRK_ALTCLK) { // if it's a clock track
      if (!ww.datablock) {
         block.t_blockstart = t_pulse_start; // just starting a data block
         ww.datablock = true; }
      ww.t_lastclkpulsestart = t_pulse_start;
      if (wwtype == WWTRK_PRICLK) ww.t_lastpriclkpulsestart = t_pulse_start;
      if (wwtype == WWTRK_ALTCLK) ww.t_lastaltclkpulsestart = t_pulse_start;
      // careful: adjust the clock based on consecutive clock pulse starts for the same track, or else track skew will make it wrong
      if (t_pulse_start - t->t_prevlastpeak < ww.clkavg.t_bitspaceavg * WW_PEAKSFAR_BITS) // if the last clock pulse start is reasonable
         adjust_clock(&ww.clkavg, (float)(t_pulse_start - t->t_prevlastpeak), t->trknum);   // then adjust our clock, incrementally}
   } }

void ww_pulse_end(struct trkstate_t *t, double t_pulse_end) { // detected the second half of a flux change pulse
   enum wwtrk_t wwtype = ww_trk_to_type[t->trknum];
   dlogtrk("trk %d type %c, ww_pulse_end   at "TIMEFMT", lastpeak "TIMEFMT", prevlastpeak "TIMEFMT", bitspaceavg %.1f\n",
           t->trknum, WWTRKTYPE_SYMBOLS[wwtype], TIMETICK(t_pulse_end), TIMETICK(t->t_lastpeak), TIMETICK(t->t_prevlastpeak), ww.clkavg.t_bitspaceavg*1e6f);
   if (doing_deskew) accumulate_avg_height(t);
   adjust_agc(t);
   t->t_lastpulseend = t_pulse_end;
   if (ww.t_lastpriclkpulseend > 0) {
      // record skew statistics about the position of this pulse end relative to the last primary clock pulse end
      float delta = (float)(t_pulse_end - ww.t_lastpriclkpulseend);
      float bitspace = ww.clkavg.t_bitspaceavg;
      if (delta > -bitspace * 1.5 && delta < bitspace * 1.5) { // not too far away
         // create a delta which is nominally the bit spacing
         if (delta <= 0 // draw it out to see the four different cases!
               || (delta > 0 && delta < bitspace * 0.5)) delta += bitspace;
         if (0 && trace_on)
            dlog("  for peakstat trk %d, bitspaceavg %.1f, t_pulse_end "TIMEFMT" ww.t_lastpriclkpulseend "TIMEFMT" delta %.1f\n",
                 t->trknum, bitspace*1e6, TIMETICK(t_pulse_end), TIMETICK(ww.t_lastpriclkpulseend), delta*1e6);
         record_peakstat(bitspace, delta, t->trknum); } }

   if (wwtype == WWTRK_PRICLK || wwtype == WWTRK_ALTCLK) { // a clock pulse end
      if (t_pulse_end - ww.t_lastclkpulseend > ww.clkavg.t_bitspaceavg * WW_PEAKSCLOSE_BITS) // not close enough to be the other clock
         ww_chk_databits(t_pulse_end); // check for data bit starts
      ww.t_lastclkpulseend = t_pulse_end; }

   if (wwtype == WWTRK_PRICLK) { // if this is the primary clock pulse end
      ww.t_lastpriclkpulseend = t_pulse_end; // record last primary clock pulse end for skew calcs
      if (ww.t_lastaltclkpulsestart > 0 && ww.t_lastaltclkpulsestart < t_pulse_end - ww.clkavg.t_bitspaceavg) { // warn if there wasn't also an alternate clock
         ++block.results[block.parmset].ww_missing_clock;
         if (verbose_level & VL_WARNING_DETAIL && !doing_deskew)
            rlog("  missing alternate clk at "TIMEFMT", last alt clk "TIMEFMT", bitspacing %.1f\n",
                 TIMETICK(t_pulse_end), TIMETICK(ww.t_lastaltclkpulsestart), ww.clkavg.t_bitspaceavg * 1e6); } }

   if (wwtype == WWTRK_ALTCLK) {  // if it is on the alternate clock track
      if (ww.t_lastpriclkpulsestart > 0 && ww.t_lastpriclkpulsestart < t_pulse_end - ww.clkavg.t_bitspaceavg) { // warn if there wasn't also a primary clock
         ++block.results[block.parmset].ww_missing_clock;
         if (verbose_level & VL_WARNING_DETAIL && !doing_deskew)
            rlog("  missing primary clk   at "TIMEFMT", last pri clk "TIMEFMT", bitspacing %.1f\n",
                 TIMETICK(t_pulse_end), TIMETICK(ww.t_lastpriclkpulsestart), ww.clkavg.t_bitspaceavg * 1e6); } }

   if (wwtype == WWTRK_PRILSB || wwtype == WWTRK_ALTLSB) {  // if it is on an odd data track
      dlogtrk("trk %d blockmark check, t_pulse_end "TIMEFMT", lastclkpulsestart "TIMEFMT", lastblockmark "TIMEFMT"\n",
              t->trknum, TIMETICK(t_pulse_end), TIMETICK(ww.t_lastclkpulsestart), TIMETICK(ww.t_lastblockmark));
      if (ww.t_lastclkpulsestart == 0  // if we have no clock (and so not in a data block)
            && t_pulse_end - ww.t_lastblockmark > ww.clkavg.t_bitspaceavg) { // and it's not close to the last blockmark
         ww.t_lastblockmark = t_pulse_end;  // then it must be a blockmark
         block.t_blockstart = t_pulse_end - ww.clkavg.t_bitspaceavg / 2; // that started about a half bit ago
         ww_blockmark(); } } }


// Whirlwind tapes vary in the flux transition polarity. Most often the negative pulse comes first, then the positive.
// But sometimes it's the opposite, and sometimes it changes mid-tape!
// The "-fluxdir=auto" option has us try to determine the polarity at the start of each block.
// The default is "-fluxdir=neg", but "-fluxdir=pos" is also available.
// The consequences of using the wrong polarity is that the data will be wrong, and there may or may not be any
// indication of that.

// The code below exchanges top/bot peaks detected in decoder.c with the pulse start/end used above, as appropriate.

void set_flux_direction(int trknum, enum flux_direction_t direction) {
   if (flux_direction_current != direction) {
      if (flux_direction_current != FLUX_AUTO) ++num_flux_polarity_changes;  // polarity changed mid-tape!
      flux_direction_current = direction;
      rlog("  the flux direction was set to %s based on a peak on track %d at time %.8lf\n\n",
           direction == FLUX_NEG ? "negative" : "positive", trknum, timenow);
      dlog("    timenow "TIMEFMT", lastpeak "TIMEFMT", bitspaceavg %.2f\n",
           TIMETICK(timenow), TIMETICK(ww.t_lastpeak), ww.clkavg.t_bitspaceavg*1e6f); } }

void ww_bot(struct trkstate_t *t) { // detected a bottom
   if (flux_direction_requested == FLUX_AUTO) {
      if (t->t_bot - ww.t_lastpeak > ww.clkavg.t_bitspaceavg * WW_PEAKSFAR_BITS) // if we've seen nothing for a while
         set_flux_direction(t->trknum, FLUX_NEG); }
   else flux_direction_current = flux_direction_requested;
   ww.t_lastpeak = t->t_bot;
   if (flux_direction_current == FLUX_NEG) ww_pulse_start(t, t->t_bot);
   else if (flux_direction_current == FLUX_POS) ww_pulse_end(t, t->t_bot);
   else assert(false, "bad flux_direction in ww_bot: %d", flux_direction_current); }

void ww_top(struct trkstate_t *t) { // detected a top
   if (flux_direction_requested == FLUX_AUTO) {
      if (t->t_top - ww.t_lastpeak > ww.clkavg.t_bitspaceavg * WW_PEAKSFAR_BITS) // if we've seen nothing for a while
         set_flux_direction(t->trknum, FLUX_POS); }
   else flux_direction_current = flux_direction_requested;
   ww.t_lastpeak = t->t_top;
   if (flux_direction_current == FLUX_NEG) ww_pulse_end(t, t->t_top);
   else if (flux_direction_current == FLUX_POS) ww_pulse_start(t, t->t_top);
   else assert(false, "bad flux_direction in ww_top: %d", flux_direction_current); }

//*
