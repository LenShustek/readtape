//file: decode_gcr.c
/******************************************************************************

   decode routines specific to 6250 BPI GCR tape format,
   which conforms to ANSI X3.54

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

#define SHOW_GCRDATA 0        // show the storage/data groups?
#define CHK_TRK_ENDING 0      // check which tracks end with a good postable?
#define KNOW_GOODDATA 0       // compare to known good data?
#define DUMP_PEAKDATA 0       // write a file with peak timing, zero bit counts, and clock rate?
#define DUMP_DATA 0           // write a file with data and ECC?
#define DUMP_DATA_1BIT 0      // if so, only write data with a single 1-bit?

static byte gcr_sgroup[9]; // 5-bit codes for tracks 0..9
static byte gcr_dgroup[9]; // 4-bit codes for tracks 0..9
static int gcr_bitnum, gcr_bytenum;
bool gcr_sequence_err; // got GCR sequence error
int gcr_bad_dgroups;

#if DUMP_PEAKDATA
#define MAXPEAKS 10000
static byte zerocounts[9][MAXPEAKS];
static float peakdeltas[9][MAXPEAKS] = { 0 };
static float bitspaceavgs[9][MAXPEAKS] = { 0 };
static int datacounts[9][MAXPEAKS] = { 0 };
static int peakcounts[9] = { 0 };
void zerocounts_init(void) {
   for (int i = 0; i < MAXPEAKS; ++i)
      for (int t = 0; t < 9; ++t) zerocounts[t][i] = 0xff; }
void zerocount(int t, int datacount, float delta, int numzeroes, float bitspaceavg) {
   static bool did_init = false;
   if (!did_init) {
      zerocounts_init();
      did_init = true; }
   if (peakcounts[t] < MAXPEAKS - 1) {
      datacounts[t][peakcounts[t]] = datacount;
      peakdeltas[t][peakcounts[t]] = delta;
      zerocounts[t][peakcounts[t]] = numzeroes;
      bitspaceavgs[t][peakcounts[t]] = bitspaceavg;
      ++peakcounts[t]; } }
void zerocounts_dump(void) {
   FILE *zfile;
   char peakfilename[MAXPATH];
   sprintf(peakfilename, "%s.peakcounts.csv", baseoutfilename);
   assert((zfile = fopen(peakfilename, "w")) != NULLP, "Unable to open zerocounts file name \"%s\"", peakfilename);
   int maxcount = 0;
   for (int t = 0; t < 9; ++t) fprintf(zfile, "cnt%d, ", t);
   for (int t = 0; t < 9; ++t) fprintf(zfile, "delta%d, ", t);
   for (int t = 0; t < 9; ++t) fprintf(zfile, "zero%d, ", t);
   for (int t = 0; t < 9; ++t) {
      if (peakcounts[t] > maxcount) maxcount = peakcounts[t];
      fprintf(zfile, "bitsp%d%s", t, t < 8 ? ", " : ""); }
   fprintf(zfile, "\n");
   for (int i = 0; i < maxcount; ++i) {
      for (int t = 0; t < 9; ++t)
         fprintf(zfile, "%d, ", datacounts[t][i]);
      for (int t = 0; t < 9; ++t)
         fprintf(zfile, "%.2f, ", peakdeltas[t][i] * 1e6);
      for (int t = 0; t < 9; ++t)
         fprintf(zfile, "%d, ", zerocounts[t][i]);
      for (int t = 0; t < 9; ++t)
         fprintf(zfile, "%.2f%s", bitspaceavgs[t][i]*1e6, t < 8 ? ", " : "");
      fprintf(zfile, "\n"); }
   fclose(zfile); }
#endif

#if CHK_TRK_ENDING
bool trk_ends_ok[9];
void chk_trk_endings(void) {
   // check the last 17 groups (85 bits) of each track: MARK2, 14xSYNC, SECOND, TERML1/TERML2
   for (int trk = 0; trk < 9; ++trk) {
      struct trkstate_t *t = &trkstate[trk];
      trk_ends_ok[trk] = false;
      rlog("trk %d datacount %d\n", trk, t->datacount);
      if (t->datacount <= 85) goto bad;
      int bitnum;
      // compensate for the fact that TERML0 tracks have two fewer bits, because they're both zero with no ending peak
      int dontcount = (data[t->datacount - 6] >> (ntrks - 1 - t->trknum)) & 1 ? 2 : 0;
      for (bitnum = 0; bitnum < 85-dontcount; ++bitnum) {
         uint16_t trkbit = (data[t->datacount - (85-dontcount) + bitnum] >> (ntrks - 1 - t->trknum)) & 1;
         uint16_t goodbit =
            // 1 zero bits in the starting MARK2, 1 in the SECOND, and 2 in the ending TERML1
            (bitnum == 3 || bitnum == 4 || bitnum == 79 || bitnum == 81 || bitnum == 83) ? 0 : 1;
         //if (trk == 1) rlog("trk %d at bitnum %d data index %d data %03X is bit %d, want bit %d\n",
         //                     trk, bitnum, t->datacount - 80 + bitnum, data[t->datacount - 80 + bitnum], trkbit, goodbit);
         if (trkbit != goodbit) goto bad; }
      trk_ends_ok[trk] = true;
bad:; } }
#endif

int first_parity_err; //TEMP

void gcr_showdata(char *title) { // display the 8 bytes we just generated
#if SHOW_GCRDATA
   rlog("%s: ", title);
   for (int i = 8; i > 1; --i)  // 7 data bytes
      rlog("%02X%c ", data[gcr_bytenum - i] >> 1, parity(data[gcr_bytenum - i]) ? ' ' : '!');
   rlog("  ECC: %02X%c", data[gcr_bytenum - 1] >> 1, parity(data[gcr_bytenum - 1]) ? ' ' : '!');
   rlog(" at %.7lf\n", data_time[gcr_bytenum - 1]);
#endif
   for (int i = 8; i > 0; --i) // TEMP
      if (parity(data[gcr_bytenum - i]) == 0 && first_parity_err == -1) first_parity_err = gcr_bytenum; }

byte eccdata[MAXBLOCK];
int eccdatacount = 0;

void gcr_write_ecc_data(void) {
#if DUMP_DATA //write a file with data + ECC
   static bool fileopen = false;
   static FILE *dataf;
   static eccwriteblks = 0, eccwritebytes = 0;
   extern byte eccdata[];
   extern int eccdatacount;
   if (!fileopen) {
      char eccfilename[MAXPATH];
      sprintf(eccfilename, "%s.eccdata.bin", baseoutfilename);
      assert((dataf = fopen(eccfilename, "wb")) != NULLP, "Unable to open ecc data file name \"%s\"", eccfilename);
      fileopen = true; }
   assert(fwrite(eccdata, 1, eccdatacount, dataf) == eccdatacount, "bad eccdata.bin write");
   ++eccwriteblks; eccwritebytes += eccdatacount;
   rlog("dumped; eccdatacount %d, eccwriteblks %d, eccwritebytes %d\n", eccdatacount, eccwriteblks, eccwritebytes);
   eccdatacount = 0;
#endif
}

void gcr_savedata(void) { // save data with ECC for writing to a file later
#if DUMP_DATA
#if DUMP_DATA_1BIT // filter for number of one bits in the data block
   int numones = 0;
   for (int i = 8; i > 1; --i) { // 7 data bytes only
      byte v = data[gcr_bytenum - i] >> 1; // the byte without parity
      // This is Brian Kernighan's clever method of counting one bits in an integer
      for (; v; ++numones) v &= v - 1; //clr least significant bit set
   }
   if (numones == 1)
#endif
   {
      for (int i = 8; i > 0; --i) // buffer 7 data bytes plus ECC, without parity
         eccdata[eccdatacount++] = data[gcr_bytenum - i] >> 1; }
#endif
}


/********************************************************************************************
   post-processing

   We have accumulated the raw ("storage") data in data[], and now we handle the group recoding
   into user data, recognize special control bytes, and check parity, ECC, and LRC.

**********************************************************************************************/

// special GCR 5-bit codes
#define GCR_MARK1    0b00111
#define GCR_MARK2    0b11100
#define GCR_SYNC     0b11111  // same as ENDMARK!
#define GCR_TERML1   0b10101  // also data 5
#define GCR_TERML0   0b10100
#define GCR_SECOND1  0b01111  // also data 15
#define GCR_SECOND2  0b11110  // also data 12

// map from GCR 5-bit code to 4-bit data values
byte gcr_data[32] = { // 255 means "invalid"
   255, 255, 255, 255, 255, 255, 255,
   255, 255, 9, 10, 11, 255, 13, 14,
   15, 255, 255, 2, 3, 255, 5,
   6, 7, 255, 0, 8, 1, 255, 4, 12, 255 };

char *gcr_sgroup_name[32] = {
   "bad0", "bad1", "bad2", "bad3", "bad4", "bad5", "bad6",
   "MARK1", "bad8", "9", "10", "11", "bad12", "13", "14",
   "SEC1/15", "bad16", "bad17", "2", "3", "TERML0", "TERML1/5",
   "6", "7", "bad24", "0", "8", "1", "MARK2", "4", "SEC2/12",
   "SYNC" };

void gcr_get_sgroups(void) {
   // gather 5 consecutive bits at gcr_bitnum to a 5-bit storage subgroup on each track
   int bitnum, trk;
   uint16_t dataword;
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   for (bitnum = 0; bitnum < 5; ++bitnum) {
      dataword = data[gcr_bitnum + bitnum];
      trk = 9; do {
         gcr_sgroup[trk - 1] = (gcr_sgroup[trk - 1] << 1) & 31 | (dataword & 1);
         dataword >>= 1; }
      while (--trk); } }

void gcr_write_dgroups(void) {
   // convert the 5-bit storage groups to 4-bits of data on each track at gcr_bytenum
   int bitnum, trk;
   uint16_t mask;
   byte nibble;
   trk = 8; mask = 1; do {
      nibble = gcr_data[gcr_sgroup[trk]];
      if (nibble == 255) {
         // rlog(" bad dgroup at trk %d gcr_bitnum %d: %02X\n", trk, gcr_bitnum, gcr_sgroup[trk]);
         ++gcr_bad_dgroups;
         nibble = 0; }
      bitnum = 3; do {
         if (nibble & 1)
            data[gcr_bytenum + bitnum] |= mask;
         else data[gcr_bytenum + bitnum] &= ~mask;
         nibble >>= 1; }
      while (--bitnum >= 0);
      mask <<= 1; }
   while (--trk >= 0);
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   bitnum = 3; do { // check for odd parity in all four 9-bit bytes we created,
      // including the ECC that will be discarded, which is the 4th byte of group B
      if (parity(data[gcr_bytenum + bitnum]) != expected_parity) ++result->vparity_errs; }
   while (--bitnum >= 0);
   gcr_bytenum += 4; }

enum gcr_state_t { // state machine for decoding blocks
   GCR_preamble, GCR_data, GCR_resync, GCR_residual, GCR_crc, GCR_postamble };

void gcr_postprocess(void) {
   // We got a good raw "6250" GCR block in data[], so try to decode it.
   // The data is rewritten back to data[] in place, but is always smaller.
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
#if DUMP_PEAKDATA
   zerocounts_dump();
#endif
#if CHK_TRK_ENDING
   chk_trk_endings();
#endif
#if KNOW_GOODDATA
   rlog("*** RAW DATA ***\n");
   rlog("     7    6    5    4    3    2    1    0    P\n");
   for (int i = 0; i < trkstate[0].datacount; ++i) {
      if (i % 5 == 0) rlog("\n");
      rlog("%3d: ", i);
      for (int bit = 0; bit < 9; ++bit)
         rlog("%d    ", (data[i] >> (8 - bit)) & 1);
      extern uint16_t gooddata[]; //TEMP
      extern int gooddatacount;
      if (i<gooddatacount) rlog("  ours: %03X good: %03X  %s", data[i], gooddata[i], data[i] == gooddata[i] ? "" : "*** BAD"); //TEMP
      rlog("\n"); }
#endif
   eccdatacount = 0;
   result->blktype = BS_BLOCK;
   result->vparity_errs = 0;
   gcr_sequence_err = false;
   gcr_bad_dgroups = 0;
   gcr_bitnum = 0;   // where we read from in data[]
   enum gcr_state_t state = GCR_preamble;
   bool groupa = true;
#if SHOW_GCRDATA
   rlog("  data counts:");
   for (int trk = 0; trk < 9; ++trk) rlog("%10d", trkstate[trk].datacount);
   rlog("\n");
#endif
   while (gcr_bitnum <= result->maxbits - 5) {
      gcr_get_sgroups(); // get another 5-bit sgroup for all tracks
      gcr_bitnum += 5;
#if SHOW_GCRDATA
      rlog("   %4d, %4d: ", gcr_bitnum-5, gcr_bytenum-4);
      for (int trk = 0; trk < 9; ++trk) // print all sgroups
         rlog("  %8s", gcr_sgroup_name[gcr_sgroup[trk]]);
      rlog("\n");
#endif
      if (state == GCR_preamble && gcr_sgroup[0] == GCR_MARK1) { //TODO why track 0? look at multiple?
         state = GCR_data;
         gcr_bytenum = 0; // where we write decoded data to in data[]
         //rlog("start data at gcr_bitnum %d\n", gcr_bitnum);
      }

      else if (state == GCR_data) {
         if (gcr_sgroup[0] == GCR_SYNC) { // end of data in this block
            state = GCR_residual;
            //rlog("end data at gcr_bitnum %d\n", gcr_bitnum);
         }
         else if (gcr_sgroup[0] == GCR_MARK2) { // temporary switch to resync burst
            state = GCR_resync;
            if (SHOW_GCRDATA) rlog("resync at gcr_bitnum %d\n", gcr_bitnum); }
         else { // still in data
            gcr_write_dgroups();
            if (!groupa) {
               // insert check/use of ECC here
               gcr_showdata("data"); // show all 8 data bytes
               gcr_savedata();
               gcr_bytenum -= 1; // remove ECC
            }
            groupa = !groupa; } }

      else if (state == GCR_resync) {
         if (gcr_sgroup[0] == GCR_MARK1) {
            state = GCR_data;
            if (SHOW_GCRDATA) rlog("end resync at gcr_bitnum %d\n", gcr_bitnum); } }

      else if (state == GCR_residual) {
         gcr_write_dgroups();
         if (!groupa) {
            gcr_showdata("residual");  // HHHH HHNE
            // leave all 8 bytes there, temporarily
            state = GCR_crc; }
         groupa = !groupa; }

      else if (state == GCR_crc) {
         gcr_write_dgroups();
         if (!groupa) {
            gcr_showdata("crc"); // BCCC CCXE
            // insert crc processing here
            int residual_count = data[gcr_bytenum - 2] /* "residual char" (X) in CRC group */ >> (5+1) /* includes parity! */;
            //rlog("residual char = %02X; adding %d of the residual bytes\n", data[gcr_bytenum - 2] >> 1, residual_count);
            // remove the residual and crc data groups from the data, except for any valid residual bytes
            gcr_bytenum -= (16 - residual_count);
            state = GCR_postamble; }
         groupa = !groupa; }

      // We ignore all sorts of other combinations. Some are bad, like a group other than SYNC or MARK1 during resync.
      // Some are good, like the MARK2, SYNC, SECOND, and TERML0/TERML1 groups during the postamble.
      // We clearly could do more error checking.

   }
#if SHOW_GCRDATA
   rlog("%d trailing bits:", result->maxbits - gcr_bitnum);
   while (gcr_bitnum < result->maxbits)
      rlog(" %03X", data[gcr_bitnum++]);
   rlog("\n");
   rlog("  data counts:");
   for (int trk = 0; trk < 9; ++trk) rlog("%10d", trkstate[trk].datacount);
   rlog("\n");
#if CHK_TRK_ENDING
   rlog("trk ending status: ");
   for (int trk = 0; trk < 9; ++trk)
      rlog("%d:%s ", trk, trk_ends_ok[trk] ? "ok " : "bad");
   rlog("\n");
#endif
#endif
   result->minbits = result->maxbits = gcr_bytenum;
   result->errcount = result->vparity_errs;
   if (gcr_bad_dgroups) {
      //rlog("   found %d bad dgroup codes using parmset %d at time %.7lf\n", gcr_bad_dgroups, block.parmset, timenow);
      ++result->errcount; }
   if (gcr_sequence_err) {
      rlog("   GCR sequence error\n");
      ++result->errcount; }
   interblock_expiration = timenow + GCR_IBG_SECS;  // ignore data for a while until we're well into the IBG
}

void show_clock_averages(void) {
   rlog("clock avgs at %.7lf tick %.1lf  ", timenow, TICK(timenow));
   for (int trk = 0; trk < ntrks; ++trk)
      rlog("%4d:%.2f ", trkstate[trk].datacount, trkstate[trk].clkavg.t_bitspaceavg*1e6);
   rlog("\n"); }

void gcr_end_of_block(void) {
   //show_clock_averages();
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   float avg_bit_spacing = 0;
   result->minbits = MAXBLOCK;
   result->maxbits = 0;
   for (int trk = 0; trk < ntrks; ++trk) { //
      struct trkstate_t *t = &trkstate[trk];
      avg_bit_spacing += (float)(t->t_lastbit - t->t_firstbit) / t->datacount;
      if (t->datacount > result->maxbits) result->maxbits = t->datacount;
      if (t->datacount < result->minbits) result->minbits = t->datacount;
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain; }
   result->avg_bit_spacing = avg_bit_spacing / ntrks;
   //rlog("ending bitspacing: "); //TEMP
   //for (int trk = 0; trk < ntrks; ++trk) rlog("%.2f ", trkstate[trk].clkavg.t_bitspaceavg*1e6); //TEMP
   //rlog("\n"); //TEMP
   dlog("GCR end_of_block, min %d max %d, avgbitspacing %.2f uS at %.7lf tick %.1lf\n",
        result->minbits, result->maxbits, result->avg_bit_spacing*1e6, timenow, TICK(timenow));
   if (result->maxbits <= 1) {  // leave result-blktype == BS_NONE //TEMP ??? changing to 10 causes problems!!!
      rlog("   ignoring noise block of length %d at %.7lf\n", result->maxbits, timenow); }
   else if (
      // "The Tape Mark is specified as 250 to 400 flux changes, all "ones," at 9042 frpi
      // in tracks 2, 5, 8, 1, 4, and 7, and no recording in tracks 3, 6, and 9." (Their numbering, not ours!)
      trkstate[0].datacount >= 250 && trkstate[0].datacount <= 400 &&
      trkstate[2].datacount >= 250 && trkstate[2].datacount <= 400 &&
      trkstate[5].datacount >= 250 && trkstate[5].datacount <= 400 &&
      trkstate[6].datacount >= 250 && trkstate[6].datacount <= 400 &&
      trkstate[7].datacount >= 250 && trkstate[7].datacount <= 400 &&
      trkstate[8].datacount >= 250 && trkstate[8].datacount <= 400 &&
      trkstate[1].peakcount <= 2 &&
      trkstate[3].peakcount <= 2 &&
      trkstate[4].peakcount <= 2) {
      result->blktype = BS_TAPEMARK; // it's a tapemark
   }
   else if (0 /*TEMP result->maxbits - result->minbits > 2*/) {  // different number of bits in different tracks
      // Note that a normal block has up to 2 bits of difference, because the last bit "restores the
      // magnetic remanence to the erase state", which means is  0 (no peak) or 1 (peak) such that
      // the last peak was positive.
      if (DEBUG) show_track_datacounts("*** malformed block");
      rlog("   malformed before post-processing, %d parity errs, with lengths %d to %d at %.7lf\n", result->vparity_errs, result->minbits, result->maxbits, timenow); }
   else gcr_postprocess(); }

void gcr_addbit(struct trkstate_t *t, byte bit, double t_bit) { // add a GCR bit
   //rlog("trk %d add bit %d to %d at %.7lf at time %.7lf, bitspacing %.2lf\n",
   //     t->trknum, bit, t->datacount, t_bit, timenow, t->clkavg.t_bitspaceavg*1e6); //TEMP
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   //if (t->datacount > 0 && t->datacount < 78) // adjust clock during preamble
   //   adjust_clock(&t->clkavg, (float)(t_bit - t->t_lastbit), t->trknum);
   t->t_lastbit = t_bit;
   if (t->datacount == 0) {
      t->t_firstbit = t_bit; // record time of first bit in the datablock
      //assert(t->v_top > t->v_bot, "v_top < v_bot in gcr_addbit at %.7lf", t_bit);
      t->max_agc_gain = t->agc_gain; }
#if 0 // we use the predefined value to start with
   if (t->v_avg_height == 0 && bit == 1) { // make our first estimate of the peak-to-peak height
      t->v_avg_height = 2 * (max(abs(t->v_bot), abs(t->v_top))); // starting avg peak-to-peak is twice the first top or bottom
      if (t->trknum == TRACETRK)
         dlog("trk %d first avg height %.2f (v_top %.2f, v_bot %.2f) when adding %d at %.7lf\n", t->trknum, t->v_avg_height, t->v_top, t->v_bot, bit, t_bit); }
#endif
   if (!t->datablock) { // this is the begining of data for this block on this track
      t->t_lastclock = t_bit - t->clkavg.t_bitspaceavg;
      //dlog("trk %d starts a data blk at %.7lf tick %.1lf, agc=%f, clkavg=%.2f\n",
      //     t->trknum, t_bit, TICK(t_bit), t->agc_gain, t->clkavg.t_bitspaceavg*1e6);
      t->datablock = true; }
   if (TRACING /*trace_on*/)
      dlog(" [add a %d to %d bytes on trk %d at %.7lf tick %.1lf, lastpeak %.7lf tick %.1lf; now: %.7lf tick %.1lf, bitspacing %.2f, agc %.2f]\n",
           bit, t->datacount, t->trknum, t_bit, TICK(t_bit), t->t_lastpeak, TICK(t->t_lastpeak),
           timenow, TICK(timenow), t->clkavg.t_bitspaceavg*1e6, t->agc_gain);
   uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
   data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
   data_time[t->datacount] = t_bit;
#if KNOW_GOODDATA // TEMP compare data to what it should be
   extern uint16_t gooddata[];
   extern int gooddatacount;
   static int baddatacount = 0;
   if (t->datacount < gooddatacount
         && (gooddata[t->datacount] & mask) != (data[t->datacount] & mask)
         && ++baddatacount < 100)
      rlog("trk %d bad data is %d instead of %d, datacount %d at %.7lf tick %.1lf\n",
           t->trknum, bit, (gooddata[t->datacount] >> (ntrks - 1 - t->trknum))&1, t->datacount, t_bit, TICK(t_bit));
#endif
   if (t->datacount < MAXBLOCK) ++t->datacount;

   t->lastbits = (t->lastbits << 1) | bit;
   if (t->datacount % 5 == 0) {
      if ((t->lastbits & 0x1f) == GCR_MARK2) {
         t->resync_bitcount = 1;
         if (SHOW_GCRDATA) rlog("trk %d resync at datacount %d time %.7lf tick %.1lf\n",
                                   t->trknum, t->datacount, timenow, TICK(timenow)); }
      if ((t->lastbits & 0x1f) == GCR_MARK1 && t->resync_bitcount > 0) {
         t->resync_bitcount = 0;
         if (SHOW_GCRDATA) rlog("trk %d end resync at datacount %d time %.7lf tick %.1lf\n",
                                   t->trknum, t->datacount, timenow, TICK(timenow)); } }
   if (t->resync_bitcount > 0) { // if we're in resync block
      if (t->resync_bitcount == 5) { // in the middle of it,
         force_clock(&t->clkavg, t->t_peakdelta, t->trknum);  // force clock bit spacing!
         if (SHOW_GCRDATA) rlog("trk %d force bitspace to %.2f at datacount %d time %.7lf tick %.1lf\n",
                                   t->trknum, t->t_peakdelta*1e6, t->datacount, timenow, TICK(timenow)); }
      ++t->resync_bitcount; } }

int gcr_checkzeros(struct trkstate_t *t, float delta /* since last peak */) {
   int numbits = 1;
   if (t->datablock) {
      t->t_peakdeltaprev = t->t_peakdelta;
      t->t_peakdelta = delta;
      /*if (t->trknum == TRACETRK)*/ TRACE(adjpos, t->t_lastpeak + t->t_pulse_adj, UPTICK, t);
      // if this peak is a long time after the previous one, it implies one or two intervening zero bits
      if (delta - t->t_pulse_adj > PARM.z1pt * t->clkavg.t_bitspaceavg) { // add a zero bit at its imputed position
         ++numbits;
         double zerobitloc = t->t_lastpeak + t->clkavg.t_bitspaceavg;
         gcr_addbit(t, 0, zerobitloc);
         /*if (t->trknum == TRACETRK)*/ TRACE(zerpos, zerobitloc, UPTICK, t);
         if (delta - t->t_pulse_adj > PARM.z2pt * t->clkavg.t_bitspaceavg) { // add a second zero bit at its imputed position
            ++numbits;
            zerobitloc += t->clkavg.t_bitspaceavg;
            gcr_addbit(t, 0, zerobitloc);
            /*if (t->trknum == TRACETRK)*/ TRACE(zerpos, zerobitloc, UPTICK, t); } }
      // experiments to adjust the clock rate, maybe based on the actual time between peaks and the number of bits we think it represents
      //if (t->datacount > 0 && t->datacount < 78) adjust_clock(&t->clkavg, delta/numbits, t->trknum); // during preamble only
      if (t->datacount > 3 && numbits == 1 // or: if there were no zeroes since the last peak
            && data[t->datacount - 2] & (1 << (ntrks - 1 - t->trknum)) // and also since the peak before that
         ) adjust_clock(&t->clkavg, t->t_peakdeltaprev, t->trknum);  // then adjust clock with delta of the 2nd of 3 consecutive 1-bits
      //if (t->datacount % 100 == 0) show_clock_averages();
      // calculate some fraction of how much this pulse seems delayed, so we can account for that when the next pulse comes
      t->t_pulse_adj = PARM.pulse_adj * (numbits * t->clkavg.t_bitspaceavg - delta); // how much to move this peak to the right
#if DUMP_PEAKDATA
      zerocount(t->trknum, t->datacount, delta, numbits - 1, t->clkavg.t_bitspaceavg);
#endif
#if 0 // write file with pulse adjustments for one track
      if (t->trknum == TRACETRK) {
         static bool fileopen = false;
         static FILE *dataf;
         if (!fileopen) {
            char filename[MAXPATH];
            sprintf(filename, "%s.pulseadj.csv", baseoutfilename);
            assert(dataf = fopen(filename, "w"), "Unable to open file name \"%s\"", filename);
            fileopen = true;
            fprintf(dataf, "trk %d pulseadj usec\n", TRACETRK); }
         fprintf(dataf, "%.4f\n", t->t_pulse_adj*1e6); }
#endif

      if (TRACING && (t->t_pulse_adj > 0.01e-6 || t->t_pulse_adj < -0.01e-6))
         dlog("trk %d adjust pulse by %.4f uS, numbits %d delta %.4f uS peak difference %.5f numbits %d datacount %d at %.7lf tick %.1lf\n",
              t->trknum, t->t_pulse_adj*1e6, numbits, delta*1e6, (delta - numbits * t->clkavg.t_bitspaceavg)*1e6, numbits, t->datacount, timenow, TICK(timenow)); }
   return numbits; // total number of bits, including the 1-bit
}

void gcr_bot(struct trkstate_t *t) { // detected a bottom
   if (TRACING) dlog("trk %d bot at %.7lf tick %.1lf, agc %.2f, peak delta %.2f uS\n",
                        t->trknum, t->t_bot, TICK(t->t_bot), t->agc_gain, (float)(t->t_bot - t->t_lastpeak)*1e6);
   if (PEAK_STATS && t->t_lastclock != 0)
      record_peakstat(t->clkavg.t_bitspaceavg, (float)(t->t_bot - t->t_lastpeak), t->trknum);
   int numbits = gcr_checkzeros(t, (float)(t->t_bot - t->t_lastpeak)); // see if there are zeroes to be added.
   gcr_addbit(t, 1, t->t_bot);  // add a data 1
   if (t->peakcount > AGC_ENDBASE && t->v_avg_height_count == 0) // if we're far enough into the data
      adjust_agc(t); }

void gcr_top(struct trkstate_t *t) {  // detected a topc
   if (TRACING) dlog("trk %d top at %.7lf tick %.1lf, agc %.2f, peak delta %.2f uS\n",
                        t->trknum, t->t_top, TICK(t->t_top), t->agc_gain, (float)(t->t_top - t->t_lastpeak)*1e6);
   if (PEAK_STATS && t->t_lastclock != 0)
      record_peakstat(t->clkavg.t_bitspaceavg, (float)(t->t_top - t->t_lastpeak), t->trknum);
   int numbits = gcr_checkzeros(t, (float)(t->t_top - t->t_lastpeak)); // see if there are zeroes to be added.
   gcr_addbit(t, 1, t->t_top); // add a data 1
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

#if 0 // deprecated; we switch to pulse location analysis instead
void gcr_midbit(struct trkstate_t *t) { // we're in between NRZI bit times: make decisions about the previous bits
   //dlog("midbit started for trk %d at %.7lf tick %.1lf,lastclock %.7lf tick %.1lf, trk0 %d bytes\n", //
   //     t->trknum, timenow, TICK(timenow), t->t_lastclock, TICK(t->t_lastclock), t->datacount);
   if (t->trknum == TRACETRK) TRACE(midbit, timenow, UPTICK, NULLP);
   t->t_last_midbit = timenow;
   double expected_pos, adjusted_pos;
   expected_pos = t->t_lastclock + t->clkavg.t_bitspaceavg;  // where we expected a bit should have been

   if (!t->hadbit) {  // if this track had no transition at the last clock (ie since the last midbit)
      gcr_addbit(t, 0, expected_pos); // add a zero bit at the expected position
      if (t->trknum == TRACETRK) TRACE(zerpos, expected_pos, UPTICK, NULLP);
      t->t_lastclock = expected_pos; // use it as the last clock (bit) position
      if (++t->consecutive_zeroes >= 3) { // if we had 3 or more zeroes
         t->datablock = false; // then we're at the end of the block for us
         t->idle = true;
         dlog("trk %d becomes idle, %d idle at %.7f tick %.1lf, AGC %.2f, v_now %f, v_lastpeak %f, bitspaceavg %.2f\n", //
              t->trknum, num_trks_idle+1, timenow, TICK(timenow), t->agc_gain, t->v_now, t->v_lastpeak, t->clkavg.t_bitspaceavg*1e6);
         if (++num_trks_idle >= ntrks) { // and maybe for all tracks
            //rlog("gcr_end_of_block, trk %d, at %.7lf\n", t->trknum, timenow); //TEMP
            gcr_end_of_block(); } } }

   else { // there was a transition on this track
      // use the actual position of the bit to gently adjust the clock
      adjusted_pos = expected_pos + PARM.pulse_adj * (t->t_lastpeak - expected_pos);
      float delta = (float)(adjusted_pos - t->t_lastclock);
      float oldavg;
      if (DEBUG) oldavg = t->clkavg.t_bitspaceavg;
      adjust_clock(&t->clkavg, delta, 0);  // adjust the clock rate based on the actual position
      if (TRACING)
         dlog("adjust clk on trk %d at %.7lf tick %.1lf with delta %.2fus into avg %.2fus making %.2fus, avg pos %.7lf tick %.1lf, adj pos %.7lf tick %.1lf\n", //
              t->trknum, timenow, TICK(timenow), delta*1e6, oldavg*1e6, t->clkavg.t_bitspaceavg*1e6,
              t->t_lastpeak, TICK(t->t_lastpeak), adjusted_pos, TICK(adjusted_pos)); //
      t->t_lastclock = adjusted_pos;  // use it as the last clock (bit) position
      t->hadbit = false;
      t->consecutive_zeroes = 0; }
   if (TRACING) dlog("midbit finished for trk %d at %.7lf tick %.1lf,lastclock %.7lf tick %.1lf, trk0 %d bytes\n", //
                        t->trknum, timenow, TICK(timenow), t->t_lastclock, TICK(t->t_lastclock), t->datacount); //
}
#endif
//*
