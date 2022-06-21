//file: tapread.c
/******************************************************************************

Read a SIMH .tap file for the purpose of using the readtape text
and binary dump routines to create an interpreted text file.

See textfile.c for the command-line parameters that control what is display
and for the guts of what this calls.

*******************************************************************************
Copyright (C) 2022 Len Shustek

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

static FILE *tapf;
static int nbytes = 0;

byte tapf_readbyte(void) {
   byte ch;
   if (fread(&ch, 1, 1, tapf) != 1) fatal("SIMH .tap endfile with no end-of-medium marker");
   ++nbytes;
   return ch; }

uint32_t tapf_get_marker(void) { // a 4-byte little-endian unsigned integer
   uint32_t val = 0;
   for (int sh = 0; sh < 32; sh += 8) val |= tapf_readbyte() << sh;
   return val; };

void read_tapfile(const char *basefilename) { // read the whole SIMH file
   char filename[MAXPATH];
   strncpy(filename, basefilename, MAXPATH - 5); filename[MAXPATH - 5] = '\0';
   strcat(filename, ".tap");
   tapf = fopen(filename, "rb");
   assert(tapf != NULLP, "Unable to open SIMH TAP file \"%s\"", filename);
   rlog("processing %s\n", filename);
   txtfile_open();  // create our output text file; abort on failure
   while (1) { // we read until the SIMH end marker
      uint32_t marker, length;
      marker = tapf_get_marker();
      if (marker == 0xffffffffL) {
         rlog(".tap end of medium\n");
         break; }
      if (marker == 0xfffffffeL) txtfile_erasegap();
      if (marker == 0x00000000L) txtfile_tapemark();
      else { // data record
         if (marker & 0x7f000000L) fatal(".tap bad marker: %08lX", marker);
         if ((length = marker & 0xffffffL) == 0) fatal(".tap bad record length: %08lX", marker);
         assert(length < MAXBLOCK, "SIMH .tap data record too big: %d", length);
         for (unsigned ndx = 0; ndx < length; ++ndx) // read all the bytes of this SIMH record
            data[ndx] = tapf_readbyte() << 1; // the data with a bogus parity bit on the right
         txtfile_outputrecord(length, marker & 0x80000000L /*error flag*/ ? 1 : 0, 0); // decode the block
         if (length & 1) tapf_readbyte(); // data is padded to an even number of bytes
         marker = tapf_get_marker(); // get the ending marker for data block
         if ((marker & 0xffffffL) != length) fatal("bad ending marker: %08lX at file offset %d", marker, nbytes); } }
   fclose(tapf); }

//*