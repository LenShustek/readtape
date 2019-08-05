// file: ibmlabels.c
/*****************************************************************************

routines for processing IBM standard tape labels

In addition to decoding the labels for display, we use the dataset
name in the header file to name the reconstructed data file.

---> See readtape.c for the merged change log <----

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

// The format of IBM standard labeled tape headers
// All fields not reserved are in EBCDIC characters

struct IBM_vol_t {
   char id[4]; 		   // "VOL1"
   char serno[6];		   // volume serial number
   char rsvd1[31];
   char owner[10];		// owner in EBCDIC
   char rxvd2[29]; };

struct IBM_hdr1_t {
   char id[4];		   	// "HDR1" or "EOF1" or "EOV1"
   char dsid[17];       // dataset identifier
   char serno[6];       // dataset serial number
   char volseqno[4];    // volume sequence numer
   char dsseqno[4];     // dataset sequence number
   char genno[4];       // generation number
   char genver[2];      // version number of generation
   char created[6];     // creation date as yyddd
   char expires[6];     // expiration date as yyddd
   char security[1];    // security: 0=none, 1=rwe, 2=we
   char blkcnt[6];      // block count (in EOF)
   char syscode[13];    // system code
   char rsvd1[7]; };

struct IBM_hdr2_t {
   char id[4];          // "HDR2" or "EOF2" or "EOV2"
   char recfm[1];       // F, V, or U
   char blklen[5];      // block length
   char reclen[5];      // record length
   char density[1];     // 3 for 1600 BPI
   char dspos[1];       // dataset position
   char job[17];        // job and job step id
   char recording[2];   // blank for 9-track tape: "odd, no translate"
   char controlchar[1]; // A for ASCII, M for machine, blank for none
   char rsvd1[1];
   char blkattrib[1];   // B=blocked, S=spanned, R=both, b=neither
   char rsvd2[41]; };

byte EBCDIC[256] = {/* EBCDIC to ASCII */
   /*0x*/ ' ', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?',
   /*1x*/ '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?',
   /*2x*/ '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?',
   /*3x*/ '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?',
   /*4x*/ ' ', '?', '?', '?', '?', '?', '?', '?', '?', '?', '[', '.', '<', '(', '+', '|',
   /*5x*/ '&', '?', '?', '?', '?', '?', '?', '?', '?', '?', '!', '$', '*', ')', ';', '^',
   /*6x*/ '-', '/', '?', '?', '?', '?', '?', '?', '?', '?', '|', ',', '%', '_', '>', '?',
   /*7x*/ '?', '?', '?', '?', '?', '?', '?', '?', '?', '`', ':', '#', '|', '\'', '=', '"',
   /*8x*/ '?', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', '?', '?', '?', '?', '?', '?',
   /*9x*/ '?', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', '?', '?', '?', '?', '?', '?',
   /*ax*/ '?', '~', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '?', '?', '?', '?', '?', '?',
   /*bx*/ '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?',
   /*cx*/ '{', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '?', '?', '?', '?', '?',
   /*dx*/ '}', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '?', '?', '?', '?', '?', '?',
   /*ex*/ '\\', '?', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '?', '?', '?', '?', '?', '?',
   /*fx*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '?', '?', '?', '?', '?', ' ' };

byte BCD1401[64] = { // IBM 1401 BCD to ASCII
   /*0x*/ ' ', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '#', '@', ':', '>', 't',  // t=tapemark
   /*1x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'r', ',', '%', '=', '\'', '"', // r=recordmark
   /*2x*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '!', '$', '*', ')', ';', 'd',  // d=delta symbol
   /*3x*/ '&', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '.', '?', '(', '<', 'g' };// g=groupmark
// blank is 00 in memory, but x10 on tape

bool compare4(uint16_t *d, const char *c) { // 4-character string compare ASCII to EBCDIC
   for (int i = 0; i<4; ++i)
      if (EBCDIC[d[i] >> 1] != c[i]) return false;
   return true; }

void copy_EBCDIC(byte *to, uint16_t *from, int len) { // copy and translate to ASCII
   while (--len >= 0) to[len] = EBCDIC[from[len] >> 1]; }

char *trim(char *p, int len) { // remove trailing blanks
   while (len && p[--len] == ' ') p[len] = '\0';
   return p; }

void dumpdata(uint16_t *pdata, int len, bool bcd) { // display a data block in hex and EBCDIC or BCD
   rlog("block length %d\n", len);
   for (int i = 0; i<len; ++i) {
      rlog("%02X %d ", pdata[i] >> 1, pdata[i] & 1); // data then parity
      if (i % 16 == 15) {
         rlog(" ");
         for (int j = i - 15; j <= i; ++j) rlog("%c", bcd ? BCD1401[pdata[j] >> 1] : EBCDIC[pdata[j] >> 1]);
         rlog("\n"); } }
   if (len % 16 != 0) rlog("\n"); }

bool ibm_label(void) {  // returns true if we found and processed a valid IBM label
   int length = block.results[block.parmset].minbits;
   struct results_t *result = &block.results[block.parmset];

   assert(sizeof(struct IBM_vol_t) == 80, "bad IBM vol type");
   assert(sizeof(struct IBM_hdr1_t) == 80, "bad IBM hdr1 type");
   assert(sizeof(struct IBM_hdr2_t) == 80, "bad IBM hdr2 type");

   if (length == 80) {

      if (compare4(data, "VOL1")) { // IBM volume header
         struct IBM_vol_t hdr;
         copy_EBCDIC((byte *)&hdr, data, 80);
         if (!quiet) {
            rlog("*** tape label %.4s, serno \"%.6s\", owner \"%.10s\"\n", hdr.id, trim(hdr.serno, 6), trim(hdr.owner, 10));
            if (result->errcount) rlog("--> %d errors\n", result->errcount); }
         //dumpdata(data, length, false);
         return true; }

      else if (compare4(data, "HDR1") || compare4(data, "EOF1") || compare4(data, "EOV1")) {
         struct IBM_hdr1_t hdr;
         copy_EBCDIC((byte *)&hdr, data, 80);
         if (!quiet) {
            rlog("*** tape label %.4s, dsid \"%.17s\", serno \"%.6s\", created%.6s\n",
                 hdr.id, trim(hdr.dsid, 17), trim(hdr.serno, 6), trim(hdr.created, 6));
            rlog("    volume %.4s, dataset %.4s\n", trim(hdr.volseqno, 4), trim(hdr.dsseqno, 4));
            if (compare4(data, "EOF1")) rlog("    block count %.6s, system %.13s\n", hdr.blkcnt, trim(hdr.syscode, 13));
            if (result->errcount) rlog("--> %d errors\n", result->errcount); }
         //dumpdata(data, length, false);
         if (compare4(data, "HDR1")) { // create the output file from the name in the HDR1 label
            char filename[MAXPATH];
            sprintf(filename, "%s-%03d-%.17s%c", baseoutfilename, numfiles + 1, hdr.dsid, '\0');
            for (unsigned i = (unsigned)strlen(filename); filename[i - 1] == ' '; --i) filename[i - 1] = 0;
            if (!tap_format) create_datafile(filename);
            hdr1_label = true; }
         if (compare4(data, "EOF1") && !tap_format) close_file();
         return true; }

      else if (compare4(data, "HDR2") || compare4(data, "EOF2") || compare4(data, "EOV2")) {
         struct IBM_hdr2_t hdr;
         copy_EBCDIC((byte *)&hdr, data, 80);
         if (!quiet) {
            rlog("*** tape label %.4s, RECFM=%.1s%.1s, BLKSIZE=%.5s, LRECL=%.5s\n",//
                 hdr.id, hdr.recfm, hdr.blkattrib, trim(hdr.blklen, 5), trim(hdr.reclen, 5));
            rlog("    job: \"%.17s\"\n", trim(hdr.job, 17));
            if (result->errcount) rlog("--> %d errors\n", result->errcount); }
         //dumpdata(data, length, false);
         return true; } }

   return false; // no label found
}

//*