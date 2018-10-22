//file: textfile.c
/******************************************************************************

Create an interpreted text file from the data with numbers in hex or octal,
and/or characters in ASCII, EBCDIC, BCD, Burroughs BIC, or DEC SixBit code,
in the style of an old-fashioned memory dump.

This is derived from the standalong program "dumptap" from May 2018,
which does the same for SIMH .tap format files.

See readtape.c for the unified change log and other info.

The command-line options which apply to this module are:
      -hex          hex 8-bit numeric data
      -octal        octal 6-bit numeric data
      -ascii        ASCII 8-bit characters
      -ebcdic       IBM EBCDIC 8-bit characters
      -bcd          IBM 1401 BCD 6-bit characters
      -b5500        Burroughs B5500 Internal Code 6-bit characters
      -sixbit       DEC SixBit code (ASCII-32)
      -linesize=nn  each line shows nn bytes

The default is 80 ASCII characters per line and no numeric data.
If the options are "-octal -b5500 -linesize=20", the output looks like this:

  file:basefilename.interp.txt
  options: -HEX -B5500 -LINESIZE=20
     80: 6043212225436060004364422562606000232162   LABEL  0LUKES  0CAS
         6360606000000106110005010001061100050300  T   0016905101690530
         0000000000000000000000000000000000000000  00000000000000000000
         0000050600000005060000000000000000000000  00560005600000000000
  tape mark
  end of flie

Block with errors are indicated by a "!" before the length

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

static byte EBCDIC[256] = {/* EBCDIC to ASCII */
   /*0x*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
   /*1x*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
   /*2x*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
   /*3x*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
   /*4x*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '[', '.', '<', '(', '+', '|',
   /*5x*/ '&', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '!', '$', '*', ')', ';', '^',
   /*6x*/ '-', '/', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '|', ',', '%', '_', '>', '?',
   /*7x*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '`', ':', '#', '|', '\'', '=', '"',
   /*8x*/ ' ', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', ' ', ' ', ' ', ' ', ' ', ' ',
   /*9x*/ ' ', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', ' ', ' ', ' ', ' ', ' ', ' ',
   /*ax*/ ' ', '~', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', ' ', ' ', ' ', ' ', ' ', ' ',
   /*bx*/ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
   /*cx*/ '{', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', ' ', ' ', ' ', ' ', ' ', ' ',
   /*dx*/ '}', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', ' ', ' ', ' ', ' ', ' ', ' ',
   /*ex*/ '\\', ' ', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', ' ', ' ', ' ', ' ', ' ', ' ',
   /*fx*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ' ', ' ', ' ', ' ', ' ', ' ' };

static byte BCD1401[64] = { // IBM 1401 BCD to ASCII
   /*0x*/ ' ', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '#', '@', ':', '>', 't',   // t = tapemark
   /*1x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'r', ',', '%', '=', '\'', '"',  // r = recordmark
   /*2x*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '!', '$', '*', ')', ';', 'd',   // d = delta
   /*3x*/ '&', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '.', '?', '(', '<', 'g' }; // g = groupmark
// blank is 00 is memory, but 10 on tape

static byte Burroughs_Internal_Code[64] = {
   /*0x*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '#', '@', '?', ':', '>', '}',   // } = greater or equal
   /*1x*/ '+', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '.', '[', '&', '(', '<', '~',   // ~ = left arrow
   /*2x*/ '|', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '$', '*', '-', ')', ';', '{',   // | = multiply, { = less or equal
   /*3x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', ',', '%', '!', ']', '=', '"' }; // ! = not equal

static byte buffer[MAXLINE];
static int linecnt, numrecords, numerrors, numtapemarks;
static long long int numbytes;
static bool txtfile_isopen = false;
static FILE *txtf;

// must match enums in decoder.h
static char *chartype_options[] = { "", "-BCD", "-EBCDIC", "-ASCII", "-B5500", "-SIXBIT" };
static char *numtype_options[] = { "", "-HEX", "-OCTAL" };

static void output_char(byte ch) {
   fprintf(txtf, "%c",
           txtfile_chartype == ASC ? (isprint(ch) ? ch & 0x7f : ' ')
           : txtfile_chartype == SIXBIT ? ((ch & 0x3f)+32)
           : txtfile_chartype == EBC ? EBCDIC[ch]
           : txtfile_chartype == BCD ? BCD1401[ch & 0x3f]
           : txtfile_chartype == BUR ? Burroughs_Internal_Code[ch & 0x3f]
           : '?'); };

static void output_chars(void) {
   for (int i = 0; i < (txtfile_linesize - linecnt); ++i) fprintf(txtf, "  "); // space out to character area
   fprintf(txtf, "  "); // decorate?
   for (int i = 0; i < linecnt; ++i) output_char(buffer[i]); };

static void txtfile_open(void) {
   char filename[MAXPATH];
   sprintf(filename, "%s.interp.txt", baseoutfilename);
   assert((txtf = fopen(filename, "w")) != NULLP, "can't open interpreted text file \"%s\"", filename);
   rlog("creating file \"%s\"\n", filename);
   fprintf(txtf, "file: %s\n", filename);
   fprintf(txtf, "options: %s %s -LINESIZE=%d\n",
           numtype_options[txtfile_numtype], chartype_options[txtfile_chartype], txtfile_linesize);
   numrecords = numerrors = numtapemarks = 0;
   numbytes = 0;
   txtfile_isopen = true; }

void txtfile_tapemark(void) {
   if (!txtfile_isopen) txtfile_open();
   ++numtapemarks;
   fprintf(txtf, "tape mark\n"); }

void txtfile_outputrecord(void) {
   int length = block.results[block.parmset].minbits;
   struct results_t *result = &block.results[block.parmset];

   if (!txtfile_isopen) txtfile_open();
   ++numrecords;
   numbytes += length;
   if (result->errcount > 0) ++numerrors;
   fprintf(txtf, "%c%4d: ", result->errcount > 0 ? '!' : ' ', length); // show ! if the block had errors
   linecnt = 0;
   for (int i = 0; i < length; ++i) { // discard the parity bit track and write all the data bits
      byte ch = (byte)(data[i] >> 1);
      if (linecnt >= txtfile_linesize) { // start a new line
         if (txtfile_doboth) output_chars();
         fprintf(txtf, "\n       ");
         linecnt = 0; }
      buffer[linecnt++] = ch;
      if (txtfile_numtype == HEX) fprintf(txtf, "%02X", ch);
      else if (txtfile_numtype == OCT) fprintf(txtf, "%02o", ch & 0x3f);
      else output_char(ch); }
   if (txtfile_doboth) output_chars();
   fprintf(txtf, "\n"); }

void txtfile_close(void) {
   fprintf(txtf, "endfile\n");
   fprintf(txtf, "there were %d data blocks with %s bytes, and %d tapemarks\n", numrecords, longlongcommas(numbytes), numtapemarks);
   if (numerrors > 0) fprintf(txtf, "%d block(s) with errors were marked with a ! before the length\n", numerrors);
   else fprintf(txtf, "no blocks had errors\n");
   fclose(txtf);
   txtfile_isopen = false; }

//*
