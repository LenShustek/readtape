//file: dumptap.c
/******************************************************************************

Dump a SIMH .tap format tape image file with numbers in hex or octal,
and/or characters in ASCII, EBCDIC, BCD, or Burroughs BIC code,
in the style of an old-fashioned memory dump.

   dumptap options filename.tap

The output goes to stdout, but can be piped elsewhere.

options:  -h    hex 8-bit numeric data
          -o    octal 6-bit numeric data
          -a    ASCII 8-bit characters
          -e    IBM EBCDIC 8-bit characters
          -b    IBM 1401 BCD 6-bit characters
          -u    Burroughs B5500 Internal Code 6-bit characters
          -lnn  each line shows nn bytes

The default is 80 ASCII characters per line and no numeric data.
If the options are "-o -u -l20", the output looks like this:

  file:102784151.tap
     80: 6043212225436060004364422562606000232162   LABEL  0LUKES  0CAS
         6360606000000106110005010001061100050300  T   0016905101690530
         0000000000000000000000000000000000000000  00000000000000000000
         0000050600000005060000000000000000000000  00560005600000000000
  .tap tape mark
  .tap end of medium

*******************************************************************************

---CHANGE LOG ---

 3 May 2018, L. Shustek, written
14 May 2018, L. Shustek, add Burroughs Internal Code
18 May 2018, L. Shustek, cleanup for more stringent compilers

TODO: on "filename" line, display summary of parms used
/******************************************************************************
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

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>
typedef unsigned char byte;

byte EBCDIC[256] = {/* EBCDIC to ASCII */
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

byte BCD1401[64] = { // IBM 1401 BCD to ASCII
   /*0x*/ ' ', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '#', '@', ':', '>', 't',   // t = tapemark
   /*1x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'r', ',', '%', '=', '\'', '"',  // r = recordmark
   /*2x*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '!', '$', '*', ')', ';', 'd',   // d = delta
   /*3x*/ '&', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '.', '?', '(', '<', 'g' }; // g = groupmark
// blank is 00 is memory, but 10 on tape

byte Burroughs_Internal_Code[64] = {
   /*0x*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '#', '@', '?', ':', '>', '}',   // } = greater or equal
   /*1x*/ '+', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '.', '[', '&', '(', '<', '~',   // ~ = left arrow
   /*2x*/ '|', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '$', '*', '-', ')', ';', '{',   // | = multiply, { = less or equal
   /*3x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', ',', '%', '!', ']', '=', '"' }; // ! = not equal

#define MAXLINE 200
FILE *inf;
#define outf stdout
enum numtype_t { NONUM, HEX, OCT }; enum numtype_t numtype = NONUM;
enum chartype_t { NOCHAR, BCD, EBC, ASC, BUR }; enum chartype_t chartype = NOCHAR;
bool do_both;
int linesize = 0;
int nbytes = 0;
byte buffer[MAXLINE];
int linecnt;

void fatal(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   fprintf(stderr, "\n");
   vfprintf(stderr, msg, args);
   fprintf(stderr, "\n");
   va_end(args);
   exit(8); }

void SayUsage(void) {
   static const char *usage[] = {
      "dumptap: display contents of a SIMH .tap file",
      "use: dumptap <options> <filename>",
      "  the input file is expected to be a SIMH .tap tape image",
      "  the output is to stdout, but can be piped elsewhere",
      "options:",
      "  -b     show BCD characters",
      "  -e     show EBCDIC characters",
      "  -a     show ASCII characters",
      "  -u     show Burroughs B5500 Internal Code characters",
      "  -o     show octal numeric data",
      "  -h     show hex numeric data",
      "  -lnn   each line displays nn bytes",
      NULL };
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]); }

bool parse_option(char *option) {
   if (option[0] != '/' && option[0] != '-') return false;
   if (toupper(option[1]) == 'L') {
      int nch;
      if (sscanf(&option[2], "%d%n", &linesize, &nch) != 1
            || linesize < 4 || linesize > MAXLINE || option[2 + nch] != '\0') fatal("bad length: %s", option); }
   else if (option[2] == '\0') // single-character switches
      switch (toupper(option[1])) {
      case '?': SayUsage(); exit(1);
      case 'H': numtype = HEX; break;
      case 'O': numtype = OCT; break;
      case 'B': chartype = BCD; break;
      case 'A': chartype = ASC; break;
      case 'E': chartype = EBC; break;
      case 'U': chartype = BUR; break;
      default: goto opterror; }
   else {
opterror:  fatal("bad option: %s\n\n", option); }
   return true; }

int HandleOptions(int argc, char *argv[]) {
   /* returns the index of the first argument that is not an option */
   int firstnonoption = 0;
   for (int i = 1; i < argc; i++) {
      if (!parse_option(argv[i])) { // end of switches
         firstnonoption = i;
         break; } }
   return firstnonoption; }

byte readbyte(void) {
   byte ch;
   if (fread(&ch, 1, 1, inf) != 1) fatal("endfile with no end-of-medium marker");
   ++nbytes;
   return ch; }

uint32_t get_marker(void) { // 4-byte little-endian unsigned integer
   uint32_t val = 0;
   for (int sh = 0; sh < 32; sh += 8) val |= readbyte() << sh;
   return val; };

void output_char(byte ch) {
   fprintf(outf, "%c",
           chartype == ASC ? (isprint(ch) ? ch & 0x7f : ' ')
           : chartype == EBC ? EBCDIC[ch]
           : chartype == BCD ? BCD1401[ch & 0x3f]
           : chartype == BUR ? Burroughs_Internal_Code[ch & 0x3f]
           : '?'); };

void output_chars(void) {
   for (int i = 0; i < (linesize - linecnt); ++i) fprintf(outf, "  "); // space out to character area
   fprintf(outf, "  "); // decorate?
   for (int i = 0; i < linecnt; ++i) output_char(buffer[i]); };

int main(int argc, char *argv[]) {
   if (argc == 1) {
      SayUsage();
      exit(4); }
   int fn = HandleOptions(argc, argv);
   if (fn == 0) fatal("no filename given");
   if ((inf = fopen(argv[fn], "rb")) == NULL)
      fatal("can't open \"%s\"", argv[fn]);
   fprintf(outf, "file:%s\n", argv[fn]);
   if (chartype == NOCHAR && numtype == NONUM) chartype = ASC;
   do_both = chartype != NOCHAR && numtype != NONUM;
   if (linesize == 0) linesize = do_both ? 40 : 80;
   while (1) {
      uint32_t marker, length;
      marker = get_marker();
      if (marker == 0xffffffffL) {
         fprintf(outf, ".tap end of medium\n");
         break; }
      if (marker == 0xfffffffeL) fprintf(outf, ".tap erase gap\n");
      if (marker == 0x00000000L) fprintf(outf, ".tap tape mark\n");
      else { // data record
         if (marker & 0x7f000000L) fatal(".tap bad marker: %08lX", marker);
         if ((length = marker & 0xffffffL) == 0) fatal(".tap bad record length: %08lX", marker);
         fprintf(outf, "%c%4d: ", marker & 0x80000000L ? '!' : ' ', length); // show ! if the block had errors
         linecnt = 0;
         for (unsigned int i = 0; i < length; ++i) { // for all bytes in the data record
            byte ch = readbyte();
            if (linecnt >= linesize) { // start a new line
               if (do_both) output_chars();
               fprintf(outf, "\n       ");
               linecnt = 0; }
            buffer[linecnt++] = ch;
            if (numtype == HEX) fprintf(outf, "%02X", ch);
            else if (numtype == OCT) fprintf(outf, "%02o", ch & 0x3f);
            else output_char(ch); }
         if (do_both) output_chars();
         fprintf(outf, "\n");
         if (length & 1) readbyte(); // data is padded to an even number of bytes
         marker = get_marker();
         if ((marker & 0xffffffL) != length) fatal("bad ending marker: %08lX", marker); } }
   fclose(inf);
   fclose(outf);
   return 0; }

//*
