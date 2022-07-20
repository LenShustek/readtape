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

#define SHOW_GCRDATA false     // show the storage/data groups?
#define CHK_TRK_ENDING false   // check which tracks end with a good postable?
#define KNOW_GOODDATA false    // compare to known good data?
#define DUMP_PEAKDATA false    // write a file with peak timing, zero bit counts, and clock rate?
#define DUMP_DATA false        // write a file with data and ECC?
#define DUMP_DATA_1BIT false   // if so, only write data with a single 1-bit?

static int gcr_bitnum, gcr_bytenum;

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
         fprintf(zfile, "%.2f%s", bitspaceavgs[t][i] * 1e6, t < 8 ? ", " : "");
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
      for (bitnum = 0; bitnum < 85 - dontcount; ++bitnum) {
         uint16_t trkbit = (data[t->datacount - (85 - dontcount) + bitnum] >> (ntrks - 1 - t->trknum)) & 1;
         uint16_t goodbit =
            // 1 zero bits in the starting MARK2, 1 in the SECOND, and 2 in the ending TERML1
            (bitnum == 3 || bitnum == 4 || bitnum == 79 || bitnum == 81 || bitnum == 83) ? 0 : 1;
         //if (trk == 1) rlog("trk %d at bitnum %d data index %d data %03X is bit %d, want bit %d\n",
         //                     trk, bitnum, t->datacount - 80 + bitnum, data[t->datacount - 80 + bitnum], trkbit, goodbit);
         if (trkbit != goodbit) goto bad; }
      trk_ends_ok[trk] = true;
bad:; } }
#endif


/********************************************************************************************
   ECC routines from Tom Howell.
   Thanks, Tom, for figuring out the algorithms and writing the code!
********************************************************************************************/

//****** check the ECC

static byte dot2(uint64_t x, uint64_t y, int w) {
   /* dot product of w-bit vectors mod 2   (w <= 64) */
   int d = 0;
   x &= y;
   while (w-- > 0) {
      d ^= x;
      x >>= 1; }
   return (d & 1); }

static byte gcr_compute_ecc(void) { // Compute the expected ECC of the 7 data bytes sitting before the ECC byte we just stored.
   static uint64_t A[] = {
      0x0f6a71994c5230ULL,
      0x70110840108004ULL,
      0x5a701108401080ULL,
      0x372be95d5a7011ULL,
      0xe95d5a70110840ULL,
      0x4c523001884412ULL,
      0x2be95d5a701108ULL,
      0x5d5a7011084010ULL };
   uint64_t dblock = 0;
   // gather the 7 data bytes before the ECC, without the parity bits, as one 56-bit big-endian integer
   for (int i = 8; i > 1; --i)
      dblock = (dblock << 8) | (data[gcr_bytenum - i] >> 1);
   byte ecc = 0;
   for (int i = 0; i < 8; ++i) // generate the ECC
      ecc |= dot2(dblock, A[i], 56) << i;
   return ecc; }

//**** error correction using the ECC

// This expects the bit ordering within the 16-bit word to be (p)(msb)...(lsb),
// not our standard (msb)...(lsb)(p). We reorder before calling correct_errors().

static void Reorderb(uint8_t *p, int *q) {
   //reorder bits within a byte.
   //p points to a byte
   //q points to an array of 8 ints
   int i;
   uint8_t temp;
   temp = 0;
   for (i = 0; i < 8; i++) {
      temp |= ((*p & (1 << i)) != 0) << q[i]; }
   *p = temp;
   return; }

void Reorderw(uint16_t *p, int *q) {
   //reorder low order 9 bits within a halfword.
   //p points to a halfword
   //q points to an array of 9 ints
   int i;
   uint16_t temp;
   temp = 0;
   for (i = 0; i < 9; i++) {
      temp |= ((*p & (1 << i)) != 0) << q[i]; }
   *p = temp;
   return; }

uint8_t timesAlphap(uint8_t s) {
   // multiply a byte by alpha mod g and return the result
   // where the byte's msb is 1 and lsb is alpha**7
   static int gp = 0x39;	//generator polynomial - x**8
   uint8_t bit7;
   bit7 = (s & (uint8_t)0x80) != 0;
   s <<= 1;
   if (bit7) s ^= gp;
   return s; }

uint8_t divbyAlphap(uint8_t s) {
   // divide a byte by alpha mod g and return the result
   // where the byte's msb is 1 and lsb is alpha**7
   static int gp = 0x9c;	//generator polynomial - x**0
   uint8_t bit0;
   bit0 = (s & (uint8_t)0x01) != 0;
   s >>= 1;
   if (bit0) s ^= gp;
   return s; }

uint8_t MatrixProduct(uint8_t *M, uint8_t x) {
   // compute Mx where M is an 8x8 bit matrix represented as 8 bytes which are rows are M
   // and x is an 8-bit vector
   // The 0th elements of each row and the vector x are the msb (as normally printed)
   int i, j;
   uint8_t ans, mask, prod, sum;
   ans = (uint8_t)0;
   for (i = 0; i < 8; i++) {
      prod = M[i] & x;
      sum = (uint8_t)0;
      mask = (uint8_t)1;
      for (j = 0; j < 8; j++) {
         sum ^= (prod & mask) != 0;
         mask <<= 1; }
      ans |= sum << (7 - i);	//fill it left to right
   }
   return ans; }

void set_bad_track_numbers(uint16_t bad_tracks, int *pi, int *pj) {
   //set pi and pj
   uint16_t i, count = 0;
   uint16_t bit_ptr = 1;
   *pi = 0;		//default
   *pj = 0;		//default
   for (i = 0; i < 9; i++) {
      if (count == 0 && (bit_ptr & bad_tracks) != 0) {
         *pi = i;
         *pj = i;
         count++; }
      else if (count == 1 && (bit_ptr & bad_tracks) != 0) {
         *pj = i;
         count++; }
      else if (count > 1 && (bit_ptr & bad_tracks) != 0) {
         rlog("Too many bad track pointers in gcr_correct_errors.  Ignoring track %d\n", i); }
      bit_ptr <<= 1; }
   return; }

// This function was requested by Len Shustek
bool correct_errors(uint16_t *dblock, uint16_t bad_tracks) {
   // These constants encode matrices used in the two-track correction algorithm
   // M1 through M7 from Table 3
   uint8_t Ms[8][8] = {
      { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
      { 0xfe, 0xfc, 0xf8, 0x0f, 0xe0, 0x3f, 0x7f, 0xff },
      { 0x54, 0xa8, 0x50, 0xf5, 0xbf, 0x2a, 0x55, 0xaa },
      { 0x93, 0x26, 0x4d, 0x09, 0x80, 0x92, 0x24, 0x49 },
      { 0xba, 0x75, 0xea, 0x6e, 0x66, 0x77, 0xee, 0xdd },
      { 0x11, 0x23, 0x46, 0x9c, 0x29, 0x42, 0x84, 0x08 },
      { 0x7c, 0xf9, 0xf3, 0x9a, 0x49, 0xef, 0xdf, 0xbe },
      { 0x39, 0x72, 0xe5, 0xf3, 0xdf, 0x87, 0x0e, 0x1c }, };

   uint8_t Mk[8];			//instead of uint64_t that is endian-dependent
   int pi, pj;				//error track pointers
   int errLoc;
   uint8_t S1p, S2p;		//syndrome halves computed with polynomials and packed bits
   uint8_t Sxp, Syp;		//scratch space for error location calculations
   uint8_t tempp, Corr_i;	//scratch space
   uint16_t B[8];			//ecc, data, parity in data order B[7] = C
   static int bitOrder[9] = { 4,2,1,5,7,3,6,0,8 };	//for set of 9 tracks
   static int undo[9] = { 7,2,1,5,0,3,6,4,8 };		//for set of 9 tracks
   static int reverse[8] = { 7,6,5,4,3,2,1,0 };		//for bytes
   uint8_t e1p, e2p;		//error patterns
   int i, j;

   //Change bad_tracks to correction order so it will correct properly
   Reorderw(&bad_tracks, bitOrder);
   // Start by extracting pi and pj from bad_tracks
   // These are now for correction order
   set_bad_track_numbers(bad_tracks, &pi, &pj);
   // This section allows Ms[1] ... Ms[7] to be stored efficiently and
   // unpacked when needed.  It is only needed for two-track correction.
   if (pj > pi) {
      for (j = 0; j < 8; j++) {
         Mk[j] = Ms[pj - pi][j];		//get the appropriate M into Mk
      }
      //Mk.w64 = Ms[pj-pi];
      for (i = 0; i < 8; i++) {
         Reorderb(&Mk[i], reverse);		//they get used in reverse order
      } }
   //fill B vector
   for (i = 0; i < 8; i++) {
      B[i] = dblock[i];
      Reorderw(&B[i], bitOrder);  //take word to correction order
   }
   //Compute the syndrome with polynomial method for S2p
   S1p = (uint8_t)0xff;			//we want the odd parity to cancel these 1's
   S2p = (uint8_t)0;
   for (i = 0; i < 8; i++) {		//compute S1p, S2p
      tempp = (uint8_t)0;
      for (j = 0; j < 9; j++) {
         tempp ^= (B[i] & ((uint16_t)1 << j)) != 0; }
      S1p ^= tempp << i;		//this puts B0 lsb and B7 msb
      S2p = timesAlphap(S2p);
      S2p ^= (uint8_t)(B[i] & (uint16_t)0xff); }
   Reorderb(&S2p, reverse);	// make it print with x**0 msb and x**7 lsb
   if (pi == pj) {
      // Do correction for a single track (pi = pj = anything)
      // Now find i in equation (17).  alpha**i S1p = S2p
      // I have to exchange timesAlpha and divbyAlpha in the correction code
      // probably because the order of bits in S2p has been reversed.
      // S1p and S2p are the first and second bytes of the symdrome
      errLoc = -1;		//should not happen for correctable errors
      Sxp = S1p;			//will need S1p and S2p again.  Do not change
      Syp = S2p;
      if (S1p != 0x00) {		 //there is an error
         if (S2p == 0x00) {   //error in parity only
            errLoc = 8; }
         else for (i = 0; i < 8; i++) {
               if (Syp == Sxp) {
                  errLoc = i;
                  break; }
               Sxp = divbyAlphap(Sxp);		//looking for S2p = (alpha**i)S1p
            }
         if (errLoc < 0) {
            rlog("no error location was found in gcr_correct_errors\n");  //uncorrectable error
            return false; } }
      //apply correction
      if (errLoc >= 0) {
         for (i = 0; i < 8; i++) {
            Corr_i = S1p & (uint8_t)(1 << i);
            B[i] ^= (Corr_i != 0) << errLoc; } } }
   else {		//do correction for two tracks, pi < pj
      Syp = S2p;
      // Compute (T**-pi)S2 + S1
      for (i = 0; i < pi; i++) {
         Syp = timesAlphap(Syp);		//why not divby?
      }
      Syp ^= S1p;
      e2p = (pj == 8) ? Syp : (uint8_t)0;
      if (pj != 8) {
         e2p = MatrixProduct(Mk, Syp); }
      e1p = e2p ^ S1p;
      //apply corrections
      //this method does not require the data array to be transposed
      for (i = 0; i <= 8; i++) {
         Corr_i = e1p & (uint8_t)(1 << i);
         B[i] ^= (Corr_i != 0) << pi;
         Corr_i = e2p & (uint8_t)(1 << i);
         B[i] ^= (Corr_i != 0) << pj; } }
   // Now convert back to the data byte order (from correction order)
   for (i = 0; i < 8; i++) {
      Reorderw(&B[i], undo); }
   // copy B[0], ... , B[7] into dblock for writing
   // dblock[8] is the original bad_tracks and has not been changed
   for (i = 0; i < 8; i++) {
      dblock[i] = B[i]; }
   return true; }


/********************************************************************************************
   debugging stuff for ECC
********************************************************************************************/

void gcr_showdata(char *title) { // display the 8 bytes we just generated
#if SHOW_GCRDATA
   rlog("%s: ", title);
   for (int i = 8; i > 1; --i)  // 7 data bytes
      rlog("%02X%c ", data[gcr_bytenum - i] >> 1, parity(data[gcr_bytenum - i]) ? ' ' : '!');
   rlog(" ECC: %02X%c ", data[gcr_bytenum - 1] >> 1, parity(data[gcr_bytenum - 1]) ? ' ' : '!');
   byte expected_ecc = gcr_compute_ecc();
   if (expected_ecc == data[gcr_bytenum - 1] >> 1) rlog("ok");
   else rlog("bad (expected %02X)", expected_ecc);
   rlog(" at %.8lf\n", data_time[gcr_bytenum - 1]);
#endif
}

uint16_t eccdata[MAXBLOCK];
int eccdatacount = 0;

void gcr_write_ecc_data(void) {
#if DUMP_DATA //write a file with data/parity, then ECC/parity, then badtracks indicator
   static bool fileopen = false;
   static FILE *dataf;
   static eccwriteblks = 0, eccwritebytes = 0;
   if (!fileopen) {
      char eccfilename[MAXPATH];
      sprintf(eccfilename, "%s.eccdata.bin", baseoutfilename);
      assert((dataf = fopen(eccfilename, "wb")) != NULLP, "Unable to open ecc data file name \"%s\"", eccfilename);
      fileopen = true; }
   assert(fwrite(eccdata, 2, eccdatacount, dataf) == eccdatacount, "bad eccdata.bin write");
   ++eccwriteblks; eccwritebytes += eccdatacount;
   rlog("dumped; eccdatacount %d, eccwriteblks %d, eccwritebytes %d\n", eccdatacount, eccwriteblks, eccwritebytes);
   eccdatacount = 0; }

void gcr_savedata(void) { // save data with ECC for writing to a file later
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
      // buffer 7 data bytes, then the ECC, including their parity bits, then the bad tracks indicator
//   byte parity;
      for (int i = 8; i > 0; --i) {
         eccdata[eccdatacount++] =
            // data[gcr_bytenum - i]; // parity on the right
            (data[gcr_bytenum - i] >> 1) | ((data[gcr_bytenum - i] & 1) << 8); //  parity on the left
         //      parity = (parity >> 1) | ((byte)data[gcr_bytenum - i] << 7); }
         //   eccdata[eccdatacount++] = parity;
      }
      eccdata[eccdatacount++] = 0;  // no bad tracks indicated, currently
   }
#endif
}

/********************************************************************************************
  pre-preocessing
********************************************************************************************/

void gcr_preprocess(void) {   // setup for processing of a block
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   gcr_bitnum = gcr_bytenum = 0;
   result->first_error = -1; }

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

const byte gcr_datamap[32] = { // map from GCR 5-bit storage code to 4-bit data values
   // 16+x means the 5-bit code is invalid for data, and x is the
   // mapped value of (one of) the nearest codes in Hamming distance
   16 + 10, 16 + 9, 16 + 2, 16 + 3, 16 + 5, 16 + 5, 16 + 6,
   16 + 7, 16 + 10, 9, 10, 11, 16 + 13, 13, 14,
   15, 16 + 2, 16 + 5, 2, 3, 16 + 5, 5,
   6, 7, 16 + 0, 0, 8, 1, 16 + 12, 4, 12, 16 + 15 };

char *gcr_sgroup_name[32] = { // display names of the GCR 5-bit storage codes
   "bad0", "bad1", "bad2", "bad3", "bad4", "bad5", "bad6",
   "MARK1", "bad8", "9", "10", "11", "bad12", "13", "14",
   "SEC1/15", "bad16", "bad17", "2", "3", "TERML0", "TERML1/5",
   "6", "7", "bad24", "0", "8", "1", "MARK2", "4", "SEC2/12",
   "SYNC" };

static byte gcr_sgroup[9]; // 5-bit codes for tracks 0..8
static int bad_parity_in_dgroup;

void gcr_bad_subgroup(int trk, const char *msg) {
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   if (debug_level & DB_GCRERRS) dlog(" bad dgroup at trk %d gcr_bitnum %d: %02X, %s\n", trk, gcr_bitnum, gcr_sgroup[trk], msg);
   ++result->gcr_bad_dgroups; }

void gcr_get_sgroups(void) {
   // gather 5 consecutive bits at gcr_bitnum on each track to a 5-bit storage subgroup
   int bitnum, trk;
   uint16_t dataword;
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   for (bitnum = 0; bitnum < 5; ++bitnum) {
      dataword = data[gcr_bitnum + bitnum];
      trk = 9; do {
         gcr_sgroup[trk - 1] = (gcr_sgroup[trk - 1] << 1) & (byte)0x1f | (dataword & 1);
         dataword >>= 1; }
      while (--trk); } }

#define GROUPA true
#define GROUPB false

void gcr_store_dgroups(bool groupa) {
   // convert the 5-bit storage groups to 4-bits of data on each track at gcr_bytenum
   struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
   int bitnum, trk;
   uint16_t mask;
   byte nibble;
   trk = 8; mask = 1; do {
      nibble = gcr_datamap[gcr_sgroup[trk]];
      if (nibble >= 16) { // bad code
         gcr_bad_subgroup(trk, "invalid 5-bit code");
         nibble -= 16; } // nibble-16 is the closest in Hamming distance
      bitnum = 3; do {
         if (nibble & 1)
            data[gcr_bytenum + bitnum] |= mask;
         else data[gcr_bytenum + bitnum] &= ~mask;
         nibble >>= 1; }
      while (--bitnum >= 0);
      mask <<= 1; }
   while (--trk >= 0);
   for (bitnum = 0; bitnum <= 3; ++bitnum) { // check for odd parity in all four 9-bit bytes we created,
      // including the ECC that will be discarded, which is the 4th byte of group B
      if (parity(data[gcr_bytenum + bitnum]) != expected_parity) {
         if (debug_level & DB_GCRERRS) dlog("parity err at byte %d data %03X from time %.8lf\n",
                                               gcr_bytenum + bitnum, data[gcr_bytenum + bitnum], data_time[gcr_bytenum + bitnum]);
         ++bad_parity_in_dgroup;
         if (result->first_error < 0) result->first_error = gcr_bytenum + bitnum; } }
   gcr_bytenum += 4; }

enum gcr_state_t { // state machine for decoding blocks
   GCR_preamble, GCR_data_A, GCR_data_B, GCR_resync, GCR_residual_A, GCR_residual_B, GCR_crc_A, GCR_crc_B, GCR_postamble };

#define MTRK 0  // master track for detecting special 5-bit subgroup codes
// We use track 0 (near the center of the tape) as the sentinel to recognize marker subgroups that should be on all tracks.
// We could look at the other tracks too. To verify that they're right? To use some kind of voting scheme?

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
      extern uint16_t gooddata[];
      extern int gooddatacount;
      if (i < gooddatacount) rlog("  ours: %03X good: %03X  %s", data[i], gooddata[i], data[i] == gooddata[i] ? "" : "*** BAD");
      rlog("\n"); }
#endif
   eccdatacount = 0;
   result->blktype = BS_BLOCK;
   result->first_error = -1;
   gcr_bitnum = 0;   // where we read from in data[]
   enum gcr_state_t state = GCR_preamble;
   bool groupa = true;
#if SHOW_GCRDATA
   rlog("  data counts:");
   for (int trk = 0; trk < 9; ++trk) rlog("%10d", trkstate[trk].datacount);
   rlog("\n");
#endif
   while (gcr_bitnum <= result->maxbits - 5) {
      gcr_get_sgroups(); // get 5-bit sgroups for each track
      gcr_bitnum += 5;
#if SHOW_GCRDATA
      rlog("   %4d, %4d: ", gcr_bitnum - 5, gcr_bytenum - 4);
      for (int trk = 0; trk < 9; ++trk) // print all sgroups
         rlog("  %8s", gcr_sgroup_name[gcr_sgroup[trk]]);
      rlog("\n");
#endif
      byte subgroup = gcr_sgroup[MTRK];
      switch (state) {

      case GCR_preamble:
         if (subgroup == GCR_MARK1) {
            state = GCR_data_A;  // start of data groups
            gcr_bytenum = 0; // where we write decoded data to in data[]
            //rlog("start data at gcr_bitnum %d\n", gcr_bitnum);
         } // we could check for proper preamble sequence here
         break;

      case GCR_data_A:
         if (subgroup == GCR_MARK2) { // temporary switch to resync burst
            state = GCR_resync;
            if (SHOW_GCRDATA) rlog("resync at gcr_bitnum %d\n", gcr_bitnum); }
         else if (subgroup == GCR_SYNC) { // end of data
            //rlog("end data at gcr_bitnum %d\n", gcr_bitnum);
            state = GCR_residual_A; }
         else {
            bad_parity_in_dgroup = 0;
            gcr_store_dgroups(GROUPA);
            state = GCR_data_B; }
         break;

      case GCR_data_B:
         gcr_store_dgroups(GROUPB);
         gcr_showdata("data"); // show all 8 data bytes, maybe
#if DUMP_DATA
         gcr_savedata();
#endif
         struct results_t *result = &block.results[block.parmset]; // where we put the results of this decoding
         if (gcr_compute_ecc() != data[gcr_bytenum - 1] >> 1) { // see if ECC is ok
            if (debug_level & DB_GCRERRS) dlog("ecc bad in dgroup ending at byte %d\n", gcr_bytenum - 1);
            ++result->ecc_errs;
            if (result->first_error < 0) result->first_error = gcr_bytenum - 1; }
         if (bad_parity_in_dgroup) { // see if there were any parity errors in these 8 bytes
            uint16_t my_order, tom_order[8];
            if (debug_level & DB_GCRERRS) {
               dlog("%d parity errors in dgroup ending at byte %d from time %.8lf:", bad_parity_in_dgroup, gcr_bytenum - 1, data_time[gcr_bytenum-1]);
               for (int i = 0; i < 8; ++i) {
                  my_order = data[gcr_bytenum - 8 + i];
                  dlog("  %02X %d", my_order >> 1, my_order & 1); }
               dlog("\n"); }
            if (do_correction) {
               for (int i = 0; i < 8; ++i) { //convert to p(msb)...(lsb)
                  my_order = data[gcr_bytenum - 8 + i];
                  tom_order[i] = ((my_order >> 1) & 0xff) | ((my_order & 0x01) << 8); }
               if (correct_errors(tom_order, 0x01)) {
                  if (debug_level & DB_GCRERRS) dlog("  as corrected using the ecc: ");
                  bad_parity_in_dgroup = 0;
                  for (int i = 0; i < 8; ++i) { //convert back to (msb)...(lsb)p
                     data[gcr_bytenum - 8 + i] = my_order = ((tom_order[i] & 0xff) << 1) | (tom_order[i] >> 8);
                     if (debug_level & DB_GCRERRS) dlog("  %02X %d", my_order >> 1, my_order & 1);
                     if (parity(my_order) != expected_parity) ++bad_parity_in_dgroup; }
                  if (debug_level & DB_GCRERRS) dlog("\n  now there are %d parity errors in the dgroup\n", bad_parity_in_dgroup);
                  ++result->corrected_bits;
                  if (gcr_compute_ecc() == data[gcr_bytenum - 1] >> 1) {
                     if (debug_level & DB_GCRERRS) dlog("  and the ecc is still correct\n") }
                  else {
                     if (debug_level & DB_GCRERRS) dlog("  but the ecc is now wrong!?!\n");
                     ++result->ecc_errs; } }
               else {
                  if (debug_level & DB_GCRERRS) dlog("did not correct error\n"); } }
            result->vparity_errs += bad_parity_in_dgroup; }
         gcr_bytenum -= 1; // remove ECC
         state = GCR_data_A;
         break;

      case GCR_resync:
         if (subgroup == GCR_MARK1) {
            state = GCR_data_A;
            if (SHOW_GCRDATA) rlog("end resync at gcr_bitnum %d\n", gcr_bitnum); }
         else if (subgroup != GCR_SYNC) gcr_bad_subgroup(MTRK, "other than SYNC or MARK1 during resync");
         break;

      case GCR_residual_A:
         gcr_store_dgroups(GROUPA);
         state = GCR_residual_B;
         break;

      case GCR_residual_B:
         gcr_store_dgroups(GROUPB);
         gcr_showdata("residual");  // HHHH HHNE; leave all 8 bytes there, temporarily
         state = GCR_crc_A;
         break;

      case GCR_crc_A:
         gcr_store_dgroups(GROUPA);
         state = GCR_crc_B;
         break;

      case GCR_crc_B:
         gcr_store_dgroups(GROUPB);
         gcr_showdata("crc"); // BCCC CCXE
         // insert crc processing here
         int residual_count = data[gcr_bytenum - 2] /* "residual char" (X) in CRC group */ >> (5 + 1) /* includes parity! */;
         if (SHOW_GCRDATA) rlog("residual char = %02X; adding %d of the residual bytes\n", data[gcr_bytenum - 2] >> 1, residual_count);
         // remove the residual and crc data groups from the data, except for any valid residual bytes
         gcr_bytenum -= (16 - residual_count);
         state = GCR_postamble;
         break;

      case GCR_postamble:
         // we could check for a proper postamble sequence here
         break;

      default:
         fatal("bad GCR state %d", state); } // switch

   } // while more bits

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
   interblock_counter = (int)(GCR_IBG_SECS / sample_deltat);  // ignore data for a while until we're well into the IBG
}

void show_clock_averages(void) {
   rlog("clock avgs at %.8lf tick %.1lf  ", timenow, TICK(timenow));
   for (int trk = 0; trk < ntrks; ++trk)
      rlog("%4d:%.2f ", trkstate[trk].datacount, trkstate[trk].clkavg.t_bitspaceavg*1e6);
   rlog("\n"); }

void gcr_end_of_block(void) {
   if (block.endblock_done) return;
   block.endblock_done = true;

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
      if (result->alltrk_max_agc_gain < t->max_agc_gain) result->alltrk_max_agc_gain = t->max_agc_gain;
      if (result->alltrk_min_agc_gain > t->min_agc_gain) result->alltrk_min_agc_gain = t->min_agc_gain; }
   result->avg_bit_spacing = avg_bit_spacing / ntrks;
   //rlog("ending bitspacing: ");
   //for (int trk = 0; trk < ntrks; ++trk) rlog("%.2f ", trkstate[trk].clkavg.t_bitspaceavg*1e6);
   //rlog("\n");
   dlog("GCR end of block, min %d max %d, avgbitspacing %.2f uS at %.8lf tick %.1lf\n",
        result->minbits, result->maxbits, result->avg_bit_spacing*1e6, timenow, TICK(timenow));
   set_expected_parity(result->maxbits);
   if (result->maxbits <= 10) {
      if (verbose_level & VL_ATTEMPTS) rlog("   detected noise block of length %d at %.8lf\n", result->maxbits, timenow);
      result->blktype = BS_NOISE; }
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
   else if (result->maxbits - result->minbits > 2) {  // different number of bits in different tracks
      // Note that a normal block has up to 2 bits of difference, because the last bit "restores the
      // magnetic remanence to the erase state", which means is  0 (no peak) or 1 (peak) such that
      // the last peak was positive.
      if (verbose_level & VL_TRACKLENGTHS) show_track_datacounts("*** block with mismatched tracks");
      result->track_mismatch = result->maxbits - result->minbits;
      result->blktype = BS_BADBLOCK; }
   else gcr_postprocess(); }

void gcr_addbit(struct trkstate_t *t, byte bit, double t_bit) { // add a GCR bit
   //rlog("trk %d add bit %d to %d at %.8lf at time %.8lf, bitspacing %.2lf\n",
   //     t->trknum, bit, t->datacount, t_bit, timenow, t->clkavg.t_bitspaceavg*1e6);
   TRACE(data, t_bit, bit ? UPTICK : DNTICK, t);
   //if (t->datacount > 0 && t->datacount < 78) // adjust clock during preamble
   //   adjust_clock(&t->clkavg, (float)(t_bit - t->t_lastbit), t->trknum);
   t->t_lastbit = t_bit;
   if (t->datacount == 0) {
      block.t_blockstart = t_bit;
      t->t_firstbit = t_bit; // record time of first bit in the datablock
      //assert(t->v_top > t->v_bot, "v_top < v_bot in gcr_addbit at %.8lf", t_bit);
      t->max_agc_gain = t->agc_gain; }
#if 0 // we use the predefined value to start with
   if (t->v_avg_height == 0 && bit == 1) { // make our first estimate of the peak-to-peak height
      t->v_avg_height = 2 * (max(abs(t->v_bot), abs(t->v_top))); // starting avg peak-to-peak is twice the first top or bottom
      if (t->trknum == TRACETRK)
         dlog("trk %d first avg height %.2f (v_top %.2f, v_bot %.2f) when adding %d at %.8lf\n", t->trknum, t->v_avg_height, t->v_top, t->v_bot, bit, t_bit); }
#endif
   if (!t->datablock) { // this is the beginning of data for this block on this track
      t->t_lastclock = t_bit - t->clkavg.t_bitspaceavg;
      dlog("trk %d starts a data blk with %d at %.8lf tick %.1lf, t_top %.8lf, v_top %.2f, t_bot %.8lf, v_bot %.2f, agc=%f, clkavg=%.2f, now %.8lf\n",
           t->trknum, bit, t_bit, TICK(t_bit), t->t_top, t->v_top, t->t_bot, t->v_bot, t->agc_gain, t->clkavg.t_bitspaceavg*1e6, timenow);
      t->datablock = true; }
   if (debug_level & DB_PEAKS) dlogtrk(" [add a %d to %d bytes on trk %d at %.8lf tick %.1lf, lastpeak %.8lf tick %.1lf; now: %.8lf tick %.1lf, bitspacing %.2f, agc %.2f]\n",
                                          bit, t->datacount, t->trknum, t_bit, TICK(t_bit), t->t_lastpeak, TICK(t->t_lastpeak),
                                          timenow, TICK(timenow), t->clkavg.t_bitspaceavg*1e6, t->agc_gain);
   uint16_t mask = 1 << (ntrks - 1 - t->trknum);  // update this track's bit in the data array
   data[t->datacount] = bit ? data[t->datacount] | mask : data[t->datacount] & ~mask;
   data_time[t->datacount] = t_bit;
#if KNOW_GOODDATA // compare data to what it should be
   extern uint16_t gooddata[];
   extern int gooddatacount;
   static int baddatacount = 0;
   if (t->datacount < gooddatacount
         && (gooddata[t->datacount] & mask) != (data[t->datacount] & mask)
         && ++baddatacount < 100)
      rlog("trk %d bad data is %d instead of %d, datacount %d at %.8lf tick %.1lf\n",
           t->trknum, bit, (gooddata[t->datacount] >> (ntrks - 1 - t->trknum)) & 1, t->datacount, t_bit, TICK(t_bit));
#endif
   if (t->datacount < MAXBLOCK) ++t->datacount;

   t->lastbits = (t->lastbits << 1) | bit;
   if (t->datacount % 5 == 0) {
      if ((t->lastbits & 0x1f) == GCR_MARK2) {
         t->resync_bitcount = 1;
         if (SHOW_GCRDATA) rlog("trk %d resync at datacount %d time %.8lf tick %.1lf\n",
                                   t->trknum, t->datacount, timenow, TICK(timenow)); }
      if ((t->lastbits & 0x1f) == GCR_MARK1 && t->resync_bitcount > 0) {
         t->resync_bitcount = 0;
         if (SHOW_GCRDATA) rlog("trk %d end resync at datacount %d time %.8lf tick %.1lf\n",
                                   t->trknum, t->datacount, timenow, TICK(timenow)); } }
   if (t->resync_bitcount > 0) { // if we're in resync block
      if (t->resync_bitcount == 5) { // in the middle of it,
         force_clock(&t->clkavg, t->t_peakdelta, t->trknum);  // force clock bit spacing!
         if (SHOW_GCRDATA) rlog("trk %d force bitspace to %.2f at datacount %d time %.8lf tick %.1lf\n",
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

      if (t->t_pulse_adj > 0.01e-6 || t->t_pulse_adj < -0.01e-6)
         if (debug_level & DB_PEAKS) dlogtrk("trk %d adjust pulse by %.4f uS, numbits %d delta %.4f uS peak difference %.5f numbits %d datacount %d at %.8lf tick %.1lf\n",
                                                t->trknum, t->t_pulse_adj*1e6, numbits, delta*1e6, (delta - numbits * t->clkavg.t_bitspaceavg)*1e6, numbits, t->datacount, timenow, TICK(timenow)); }
   return numbits; // total number of bits, including the 1-bit
}

void gcr_bot(struct trkstate_t *t) { // detected a bottom or zerocrossing down
   if (debug_level & DB_PEAKS) dlogtrk("trk %d dwn at %.8lf tick %.1lf, agc %.2f, peak delta %.2f uS, timenow %.8lf\n",
                                          t->trknum, t->t_bot, TICK(t->t_bot), t->agc_gain, (float)(t->t_bot - t->t_lastpeak)*1e6, timenow);
   if (PEAK_STATS && t->t_lastclock != 0)
      record_peakstat(t->clkavg.t_bitspaceavg, (float)(t->t_bot - t->t_lastpeak), t->trknum);
   int numbits = gcr_checkzeros(t, (float)(t->t_bot - t->t_lastpeak)); // see if there are zeroes to be added.
   gcr_addbit(t, 1, t->t_bot);  // add a data 1
   if (t->peakcount > AGC_ENDBASE && t->v_avg_height_count == 0) // if we're far enough into the data
      adjust_agc(t); }

void gcr_top(struct trkstate_t *t) {  // detected a top or zerocrossing up
   if (debug_level & DB_PEAKS) dlogtrk("trk %d up at %.8lf tick %.1lf, agc %.2f, peak delta %.2f uS, timenow %.8lf\n",
                                          t->trknum, t->t_top, TICK(t->t_top), t->agc_gain, (float)(t->t_top - t->t_lastpeak)*1e6, timenow);
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
         dlogtrk("trk %d avg peak-to-peak after %d transitions is %.2fV at %.8lf\n", t->trknum, AGC_ENDBASE - AGC_STARTBASE, t->v_avg_height, timenow);
         assert(t->v_avg_height > 0, "avg peak-to-peak voltage isn't positive");
         t->v_avg_height_count = 0; }
      else adjust_agc(t); // otherwise adjust AGC
   } }

//*
