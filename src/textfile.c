//file: textfile.c
/******************************************************************************

Create an interpreted text file from the data with numbers in hex or octal,
and/or characters in ASCII, EBCDIC, BCD, Burroughs BIC, or DEC SixBit code,
in the style of an old-fashioned memory dump.

This was derived from the standalong program "dumptap" from May 2018,
which does the same for SIMH .tap format files but hasn't been kept
up-to-date. This readtape program now has a -tapread option that
does the same thing: interprets a SIMH .tap file without having to
recreate it by reading and decoding the original tape data.

See readtape.c for the unified change log and other info.

The readtape command-line options which apply to this module are:
      -hex          hex 8-bit numeric byte data
      -octal        octal numeric byte data
                    (2 digits for 6- and 7-tracks, otherwise 3 digits)
      -octal2       octal 12-bit numeric word data

      -ascii        ASCII 8-bit characters
      -ebcdic       IBM EBCDIC 8-bit characters
      -bcd          IBM 1401 BCD 6-bit characters
      -b5500        Burroughs B5500 Internal Code 6-bit characters
      -sixbit       DEC SixBit code (ASCII-32)
      -SDS          Scientific Data Systems memory characters
      -SDSM         Scientific Data Systems magtape characters
      -flexo        Frieden Flexowriter terminal characters
      -adage        Adage Graphics Terminal
      -adagetape    Adage Graphics Terminal mag tape
      -CDC          CDC 7-track 6-bit code
      -Univac       UNIVAC 7-track 6-bit code

      -linesize=nn  each line shows nn bytes
      -dataspace=n  insert a space between every n bytes of data
      -linefeed     make an ASCII linefeed start a new line

The default is 80 ASCII characters per line and no numeric data.

As an example, if the options are "-octal -b5500 -linesize=20",
the output looks like this:

  file basefilename.hex.B5500.txt
  was created by readtape version 3.4 on Sat Nov 18 17:01:02 2018
  using text options -hex -B5500 -linesize=20

     80: 6043212225436060004364422562606000232162   LABEL  0LUKES  0CAS
         6360606000000106110005010001061100050300  T   0016905101690530
         0000000000000000000000000000000000000000  00000000000000000000
         0000050600000005060000000000000000000000  00560005600000000000
  tape mark
  end of flie

Blocks with errors are indicated by a "!" before the length
Blocks with warnings are indicated by a '?' before the length
Blocks with both are indicated by a 'X" before the length

*******************************************************************************
Copyright (C) 2018,2019,2022 Len Shustek

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


static byte buffer[MAXLINE];
static int bufcnt, bufstart, numrecords, numerrors, numwarnings, numerrorsandwarnings, numtapemarks, numchars;
static long long int numbytes;
static bool txtfile_isopen = false;
static FILE *txtf;

//---- stuff starting here used to be identical to what's in dumptap, but isn't anymore;
//---- in any case dumptap is rendered obsolete by the -tapread option of readtape

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
   /*0o*/ ' ', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '#', '@', ':', '>', 't',   // t = tapemark
   /*2o*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'r', ',', '%', '=', '\'', '"',  // r = recordmark
   /*4o*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '!', '$', '*', ')', ';', 'd',   // d = delta
   /*6o*/ '&', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '.', '?', '(', '<', 'g' }; // g = groupmark
// blank is 00 is memory, but 10 on tape

static byte Burroughs_Internal_Code[64] = {
   /*0o*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '#', '@', '?', ':', '>', '}',   // } = greater or equal
   /*2o*/ '+', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '.', '[', '&', '(', '<', '~',   // ~ = left arrow
   /*4o*/ '|', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '$', '*', '-', ')', ';', '{',   // | = multiply, { = less or equal
   /*6o*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', ',', '%', '!', ']', '=', '"' }; // ! = not equal

static byte SDS_Internal_Code[64] = { //
   /*0o*/ '0', '1', '2', '3', '4', '5', '6', '7',  '8', '9', '0', '=', '\'', ':', '>', 's',   // s = square root
   /*2o*/ '+', 'A', 'B', 'C', 'D', 'E', 'F', 'G',  'H', 'I', '?', '.', ')', '[', '<', 'g',    // g = group mark
   /*4o*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P',  'Q', 'R', '!', '$', '*', ']', ';', 'd',    // d = delta
   /*6o*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X',  'Y', 'Z', 'r', ',', '(', '~', '\\', '#' }; // r = record mark, # = sideways group mark

static byte SDS_Magtape_Code[64] = { // http://bitsavers.org/pdf/sds/9xx/periph/901097A_MagtapeSysTechMan.pdf
   /*0o*/ '0', '1', '2', '3', '4', '5', '6', '7',  '8', '9', '0', '#', '@', ':', '>', 's',   // s = square root
   /*2o*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X',  'Y', 'Z', 't', ',', '%', '~', '\\', 'g',  // t = tab g = group mark
   /*4o*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P',  'Q', 'R', 'c', '$', '*', ']', ';', 'd',   // c = carriage return, d = delta
   /*6o*/ '&', 'A', 'B', 'C', 'D', 'E', 'F', 'G',  'H', 'I', 'b', '.', 'l', '[', '<', 'r' }; // b = backspace, l = lozenge, r = record mark

static byte Flexowriter_Code[64] = {
   /*0o*/ ' ', ' ', 'e', '8', ' ', '|', 'a', '3', ' ', '=', 's', '4', 'i', '+', 'u', '2', // only true blank is o10, others were #
   /*2o*/ '.', '.', 'd', '5', 'r', 'l', 'j', '7', 'n', ',', 'f', '6', 'c', '-', 'k', ' ', // . is <color>
   /*4o*/ 't', ' ', 'z', '.', 'l', '.', 'w', ' ', 'h', '.', 'y', ' ', 'p', ' ', 'q', ' ', // . is \h, \t, \n
   /*60*/ 'o', '.', 'b', ' ', 'g', ' ', '9', ' ', 'm', '.', 'x', ' ', 'v', '.', '0', ' ' }; // . is <stop>, <upper>, <lower>, blank is <null>

static byte Adage_code[64] = { // Adage AGT; http://bitsavers.org/pdf/adage/agt/Adage_AGT_Programmers_Reference_Manual_Volume_I_196907.pdf, p41
   /*0o*/ ' ', '%', 'c', '!', '&', '*', ':', '_', '+', 't', '?', '"', '\'', 'r', '(', ')', //0 might be [, 2 might be ] instead of cent-sign; t=tab r=CR
   /*2o*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ';', '=', ',', '-', '.', '/',
   /*4o*/ ' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
   /*60*/ 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '$', '#', '@', '+', 'b' }; // this + is +-, b=backspace

static byte Adagetape_code[64] = { // Adage AGT magtape; http://bitsavers.org/pdf/adage/agt/Adage_AGT_Programmers_Reference_Manual_Volume_I_196907.pdf, p387
   /*0o*/ ' ', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '"', ' ', ' ', ' ', ' ',
   /*2o*/ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
   /*4o*/ 'W', 'X', 'Y', 'Z', 'u', '@', '%', ']', 'I', 'J', 'K', 'L', 'M', 'N', ' ', ' ', // u is uparrow
   /*60*/ '+', '-', '*', '/', '.', '(', ')', ',', '=', '&', ':', ' ', '$', '#', ' ', 'r' }; // r is CR

static byte CDC_code[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-*/()$= ,.#[]:\"_!&\'?<>@\\^;";
typedef char CDC_ok [sizeof(CDC_code) == 65]; // compiler check that it is 64 characters

static byte Univac_code[] = "@[]#^ ABCDEFGHIJKLMNOPQRSTUVWXYZ)-+<=>&$*(%:?!,\\0123456789';/.o~"; // o is lozenge
typedef char Univac_ok [sizeof(Univac_code) == 65]; // compiler check that it is 64 characters

// must correspond to enums in decoder.h
static char *chartype_options[] = { " ", "-BCD", "-EBCDIC", "-ASCII", "-B5500", "-sixbit", "-SDS", "-SDSM", "-flexo",
                                    "-adage", "-adagetape", "-CDC", "-Univac" };
static char *numtype_options[] = { " ", "-hex", "-octal", "-octal2" };

static void output_char(byte ch, bool oddbyte) {
   fprintf(txtf, "%c",
           txtfile_chartype == BCD ? BCD1401[ch & 0x3f] :
           txtfile_chartype == EBC ? EBCDIC[ch] :
           txtfile_chartype == ASC ? (isprint(ch & 0x7f) ? ch & 0x7f : ' ') :
           txtfile_chartype == BUR ? Burroughs_Internal_Code[ch & 0x3f] :
           txtfile_chartype == SIXBIT ? ((ch & 0x3f) + 32) :  // the 64 characters of ASCII starting at 32
           txtfile_chartype == SDS ? SDS_Internal_Code[ch & 0x3f] :
           txtfile_chartype == SDSM ? SDS_Magtape_Code[ch & 0x3f] :
           txtfile_chartype == FLEXO ? Flexowriter_Code[(oddbyte ? ch : ch>>2) & 0x3f] : // use the high and low 6 bits of a 16-bit word
           txtfile_chartype == ADAGE ? Adage_code[ch & 0x3f] :
           txtfile_chartype == ADAGETAPE ? Adagetape_code[ch & 0x3f] :
           txtfile_chartype == CDC ? CDC_code[ch & 0x3f] :
           txtfile_chartype == UNIVAC ? Univac_code[ch & 0x3f] :
           '?'); };

//---- stuff ending here used to be identical to what's in dumptap, but isn't anymore

static void output_chars(void) { // output characters for "bufcnt" bytes
   int nmissingbytes = txtfile_linesize - bufcnt;
   int nspaces = txtfile_dataspace ? nmissingbytes / txtfile_dataspace : 0;
   // for short lines, space out for missing bytes
   if (txtfile_numtype == HEX || ntrks <= 7) nspaces += nmissingbytes * 2; // "xx" or "oo"
   else /* OCT, OCt2 */ nspaces += nmissingbytes * 3; // "ooo"
   for (int i = 0; i < nspaces; ++i) fprintf(txtf, " "); // space out to character area
   if (txtfile_dataspace == 0) fprintf(txtf, "  ");
   for (int i = 0; i < bufcnt; ++i) output_char(buffer[i], (bufstart+i)&1); };

void txtfile_open(void) { // create <base>.<options>.txt file for interpreted data
   char filename[MAXPATH];
   snprintf(filename, MAXPATH, "%s.%s%s%s%stxt", baseoutfilename,
            numtype_options[txtfile_numtype] + 1, // +1 skips leading - or blank
            txtfile_doboth ? "." : "",
            chartype_options[txtfile_chartype] + 1, // +1 skips leading - or blank
            txtfile_numtype != NONUM || txtfile_chartype != NOCHAR ? "." : "");
   assert((txtf = fopen(filename, "w")) != NULLP, "can't open interpreted text file \"%s\"", filename);
   rlog("creating file \"%s\"\n", filename);
   fprintf(txtf, "file %s\n", filename);
   time_t time_now;
   time_now = time(NULL);
   fprintf(txtf, "created by readtape%s version %s on %s", tap_read ? " -tapread" : "", version_string, ctime(&time_now));
   fprintf(txtf, "using text options %s %s%s -linesize=%d",
           numtype_options[txtfile_numtype], chartype_options[txtfile_chartype],
           txtfile_linefeed ? " -newline" : "", txtfile_linesize);
   if (txtfile_dataspace) fprintf(txtf, " -dataspace=%d", txtfile_dataspace);
   if (txtfile_numtype == NONUM && txtfile_chartype == NOCHAR)
      fprintf(txtf, "\nno numeric or character options were given, so we will display only block sizes");
   fprintf(txtf, "\n\n");
   numrecords = numerrors = numwarnings =numerrorsandwarnings = numtapemarks = numchars = 0;
   numbytes = 0;
   txtfile_isopen = true; }

void txtfile_message (const char *msg, ...) {
   if (!txtfile_isopen) txtfile_open();
   if (numchars > 0) {
      fprintf(txtf, "\n");
      numchars = 0; }
   va_list args;
   va_start(args, msg);
   vfprintf(txtf, msg, args);
   va_end(args); }

void txtfile_tapemark(bool tapfile) {
   ++numtapemarks;
   txtfile_message(tapfile ? "tape mark\n" : "tape mark at time %.8lf\n", timenow); }

void txtfile_outputrecord(int length, int errs, int warnings) {
   if (!txtfile_isopen) txtfile_open();
   ++numrecords;
   numbytes += length;
   if (errs * warnings > 0) ++numerrorsandwarnings;
   else {
      if (errs > 0) ++numerrors;
      if (warnings > 0) ++numwarnings; }
   char flag = errs * warnings > 0 ? 'X' : // show X for both errors and warnings
               errs > 0 ? '!' : // show ! for errors only
               warnings > 0 ? '?' : ' ';// show ? for warnings only
   if (txtfile_numtype == NONUM && txtfile_chartype == NOCHAR) { // abbreviated display of just error and lengths
      if (numchars > 0) numchars += fprintf(txtf, ", "); // separated by commas
      numchars += fprintf(txtf, "%c%d", flag, length);
      if (numchars >= txtfile_linesize) { // and broken into -linesize lines
         fprintf(txtf, "\n");
         numchars = 0; } }
   else { // normal display of data and/or text
      if (txtfile_verbose) {
         struct results_t *result = &block.results[block.parmset];
         fprintf(txtf, "block %d: %d bytes at time %.8lf, %s\n ",
                 numblks+1, length, timenow, format_block_errors(result)); }
      else fprintf(txtf, "%c%4d: ", flag, length); // 7 chars
      bufcnt = bufstart = 0;
      for (int i = 0; i < length; ++i) { // discard the parity bit track and write only the data bits
         byte ch = (byte)(data[i] >> 1);
         byte ch2 = (byte)(data[i + 1] >> 1); // in case, for OCT2, we are doing two bytes at once
         if (bufcnt >= txtfile_linesize
               || txtfile_linefeed && ch == 0x0a) { // start a new line
            if (txtfile_doboth) output_chars();
            fprintf(txtf, txtfile_verbose ? "\n " : "\n       "); // 7 chars if not verbose
            bufcnt = 0; bufstart = i; }
         buffer[bufcnt++] = ch; // save the byte for doing character interpretation
         if (txtfile_numtype == HEX) fprintf(txtf, "%02X", ch);
         else if (txtfile_numtype == OCT // for 8-bit octal data
                  || (txtfile_numtype == OCT2 && i == length - 1)) // or an odd last byte of 16-bit words
            fprintf(txtf, ntrks <= 7 ? "%02o" : "%03o", ch); // 2 chars for 6- or 7-track, otherwise 3 chars
         else if (txtfile_numtype == OCT2) { // do this byte and the next byte together
            fprintf(txtf, "%06o", ((uint16_t)ch << 8) | ch2);
            buffer[bufcnt++] = ch2; // save another byte for character interpretation
            ++i; }
         if (txtfile_numtype != NONUM) {
            if (txtfile_dataspace > 0 && bufcnt % txtfile_dataspace == 0)
               fprintf(txtf, " "); } // extra space between groups of numeric data
         else output_char(ch, i & 1); // only doing characters, not numbers
      }
      if (txtfile_doboth) output_chars(); // do the buffered-up characters whose numerics we did
      fprintf(txtf, "\n"); } }

void txtfile_close(void) {
   if (txtfile_isopen) {
      if (numchars > 0) fprintf(txtf, "\n");
      fprintf(txtf, "end of file\n\n");
      fprintf(txtf, "there were %d data blocks with %s bytes, and %d tapemarks\n", numrecords, longlongcommas(numbytes), numtapemarks);
      if (txtfile_verbose) {
         if (numerrorsandwarnings > 0)
            fprintf(txtf, "%d block%s had both errors and warnings\n", numerrorsandwarnings, add_s(numerrorsandwarnings));
         if (numerrors > 0) fprintf(txtf, "%d block%s had errors\n", numerrors, add_s(numerrors));
         else if (numerrorsandwarnings == 0) fprintf(txtf, "no blocks had errors\n");
         if (numwarnings > 0) fprintf(txtf, "%d block%s had warnings\n", numwarnings, add_s(numwarnings));
         else if (numerrorsandwarnings == 0) fprintf(txtf, "no blocks had warnings\n"); }
      else {
         if (numerrorsandwarnings > 0)
            fprintf(txtf, numerrorsandwarnings == 1 ?
                    "%d block with both errors and warnings was marked with a X before the length\n" :
                    "%d blocks with both errors and warnings were marked with a X before the length\n", numerrorsandwarnings);
         if (numerrors > 0)
            fprintf(txtf, numerrors == 1 ?
                    "%d block with errors was marked with a ! before the length\n" :
                    "%d blocks with errors were marked with a ! before the length\n", numerrors);
         else if (numerrorsandwarnings == 0) fprintf(txtf, "no blocks had errors\n");
         if (numwarnings > 0)
            fprintf(txtf, numwarnings == 1 ?
                    "%d block with warnings was marked with a ? before the length\n" :
                    "%d blocks with warnings were marked with a ? before the length\n", numwarnings);
         else if (numerrorsandwarnings == 0) fprintf(txtf, "no blocks had warnings\n"); }
      fclose(txtf);
      txtfile_isopen = false; } }

//*
