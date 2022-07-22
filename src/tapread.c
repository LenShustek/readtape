//file: tapread.c
/******************************************************************************

Read a SIMH .tap file for the purpose of using the readtape text
and binary dump routines to create an interpreted text file.

See textfile.c for the command-line parameters that control what is displayed
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
   if (fread(&ch, 1, 1, tapf) != 1) {
      if (feof(tapf)) fatal(".tap endfile too soon");
      else fatal("error reading .tap file: %s", strerror(ferror(tapf))); }
   ++nbytes;
   return ch; }

uint32_t tapf_get_marker(void) { // a 4-byte little-endian unsigned integer
   byte chs[4];
   if (fread(&chs, 1, 4, tapf) != 4) {
      if (feof(tapf)) {  // endfile: treat as "end of medium"
         txtfile_message("missing .tap end-of-medium marker\n");
         return 0xffffffffL; }
      else fatal("error reading .tap file: %s", strerror(ferror(tapf))); }
   uint32_t val = 0;
   for (int ndx = 3; ndx >= 0; --ndx) val = (val << 8) | chs[ndx];
   return val; };

void read_tapfile(const char *basefilename, const char *extension) { // read the whole SIMH file
   char filename[MAXPATH];
   strncpy(filename, basefilename, MAXPATH - 5); filename[MAXPATH - 5] = '\0';
   strcat(filename, extension);
   tapf = fopen(filename, "rb");
   if (!tapf && !*extension) {  // if that didn't work and there wasn't an extension given
      strcat(filename, ".tap");
      tapf = fopen(filename, "rb"); }
   assert(tapf != NULLP, "Unable to open SIMH TAP file \"%s\"", filename);
   rlog("processing %s\n", filename);
   txtfile_open();  // create our output text file; abort on failure
   txtfile_verbose = false; // there is no detailed error info in .tap files
   numblks = 0;
   while (1) { // we read until we get the SIMH end marker or the end of the file
      uint32_t marker, length;
      marker = tapf_get_marker();
      if (marker == 0xffffffffL) {
         rlog(".tap end of medium\n");
         break; }
      if (marker == 0xfffffffeL) txtfile_message("erased gap\n");
      if (marker == 0x00000000L) txtfile_tapemark(true);
      else { // data record
         if (marker & 0x7f000000L) fatal(".tap bad marker: %08lX", marker);
         if ((length = marker & 0xffffffL) == 0) fatal(".tap bad record length: %08lX", marker);
         assert(length < MAXBLOCK, ".tap data record too big: %d", length);
         for (unsigned ndx = 0; ndx < length; ++ndx) // read all the bytes of this SIMH record
            data[ndx] = tapf_readbyte() << 1; // the data with a bogus parity bit on the right
         txtfile_outputrecord(length, /* error flag: */ marker & 0x80000000L ? 1 : 0, /*warning flag: */ 0); // decode the record
         // There is supposed to be exactly one byte of padding if the length is odd, following by a 4-byte trailing length that
         // matches the length at the start of the record. But some writers disobeyed the spec and didn't pad, or padded too much.
         // So we look for the matching trailing length in up to 4 places, accommodating 0 to 3 bytes of padding.
         marker = tapf_get_marker(); // get a potential trailing length for the data record assuming there is no padding
         int tries = 0;
         while ((marker & 0xffffffL) != length) {
            if (++tries > 4) fatal("didn't find .tap trailing record length at file offset %d", nbytes);
            marker = (marker >> 8) | ((uint32_t)tapf_readbyte() << 24); // skip a byte: discard LSB, add a new MSB
         }
         ++numblks; } }
   fclose(tapf); }

//*