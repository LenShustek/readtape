//file: dumptap.c
/******************************************************************************

Dump a SIMH .tap format tape image file with numbers in hex or octal,
and/or characters in ASCII, EBCDIC, BCD, SDS, or Burroughs BIC code,
in the style of an old-fashioned memory dump.

   dumptap options <basefilename>

The input is from <basefilename>.tap, and the output goes to
<basefilename>.<options>.txt, where <options> encodes the numeric and
character options.

options:  -hex      hex 8-bit numeric data
          -octal    octal 6-bit numeric data

          -ascii    ASCII 8-bit characters
          -ebcdic   IBM EBCDIC 8-bit characters
          -bcd      IBM 1401 BCD 6-bit characters
          -b5500    Burroughs B5500 Internal Code 6-bit characters
          -sixbit   DEC SixBit 6-bit characters
          -SDS      SDS internal code

          -linesize=nn  each line shows nn bytes

The default is 80 ASCII characters per line and no numeric data.
If the options are "-octal -b5500 -linesize=20", the output looks like this:

  dumptap file:102784151.diskdump 
  options: -octal -B5500 -LINESIZE=20
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
14 Oct 2018, L. Shustek, add DEC sixbit character decoding
17 Dec 2018, L. Shustek, add SDS; change parms to match readtape; display parms;
                         change file naming

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
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>
typedef unsigned char byte;


#define MAXLINE 250
FILE *inf, *txtf;
bool txtfile_doboth;
int txtfile_linesize = 0;
int nbytes = 0;
byte buffer[MAXLINE];
int linecnt;


//----------------- stuff starting here is identical to what's in readtape

// .. from decoder.h
enum txtfile_numtype_t { NONUM, HEX, OCT };
enum txtfile_chartype_t { NOCHAR, BCD, EBC, ASC, BUR, SIXBIT, SDS };

// .. from readtape.c
enum txtfile_numtype_t txtfile_numtype = NONUM;
enum txtfile_chartype_t txtfile_chartype = NOCHAR;

// .. from textfile.c
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

static byte SDS_Internal_Code[64] = {
   /*0x*/ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ' ', '=', '\'', ':', '>', 's',   // s = square root
   /*1x*/ '+', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '.', ')', '[', '<', 'g',   // g = group mark
   /*2x*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '!', '$', '*', ']', ';', 'd',   // d = delta
   /*3x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'r', ',', '(', '~', '\\', '#' }; // r = record mark, # = sideways group mark

// must correspond to enums in decoder.h
static char *chartype_options[] = { " ", "-BCD", "-EBCDIC", "-ASCII", "-B5500", "-SIXBIT", "-SDS" };
static char *numtype_options[] = { " ", "-hex", "-octal" };

static void output_char(byte ch) {
   fprintf(txtf, "%c",
           txtfile_chartype == ASC ? (isprint(ch) ? ch & 0x7f : ' ')
           : txtfile_chartype == SIXBIT ? ((ch & 0x3f) + 32)
           : txtfile_chartype == EBC ? EBCDIC[ch]
           : txtfile_chartype == BCD ? BCD1401[ch & 0x3f]
           : txtfile_chartype == BUR ? Burroughs_Internal_Code[ch & 0x3f]
           : txtfile_chartype == SDS ? SDS_Internal_Code[ch & 0x3f]
           : '?'); };

//----------------- stuff ending here is identical to what's in readtape


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
      "  the input is <filename>.tap, a SIMH tape image",
      "  the output is <filename>.<options>.txt",
      "options:",
      "  -bcd      show BCD characters",
      "  -ebcdic   show EBCDIC characters",
      "  -ascii    show ASCII characters",
      "  -B5500    show Burroughs B5500 internal code characters",
      "  -sixbit   show DEC sixbit characters",
      "  -SDS      show SDS (940, etc.) internal code characters",
      "  -octal    show octal numeric data",
      "  -hex      show hex numeric data",
      "  -linesize=nn   each line displays nn bytes",
      "the default is -ascii -linesize=80",
      NULL };
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]); }

bool opt_key(const char* arg, const char* keyword) {
   do { // check for a keyword option and nothing after it
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   return *arg == '\0'; }

bool opt_int(const char* arg, const char* keyword, int *pval, int min, int max) {
   do { // check for a "keyword=integer" option and nothing after it
      if (toupper(*arg++) != *keyword++)
         return false; }
   while (*keyword);
   int num, nch;
   if (sscanf(arg, "%d%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') return false;
   *pval = num;
   return true; }

bool parse_option(char *option) {
   char *arg = option + 1;
   if (option[0] != '-') return false;
   else if (opt_key(arg, "HEX")) txtfile_numtype = HEX;
   else if (opt_key(arg, "OCTAL")) txtfile_numtype = OCT;
   else if (opt_key(arg, "ASCII")) txtfile_chartype = ASC;
   else if (opt_key(arg, "EBCDIC")) txtfile_chartype = EBC;
   else if (opt_key(arg, "BCD")) txtfile_chartype = BCD;
   else if (opt_key(arg, "B5500")) txtfile_chartype = BUR;
   else if (opt_key(arg, "SIXBIT")) txtfile_chartype = SIXBIT;
   else if (opt_key(arg, "SDS")) txtfile_chartype = SDS;
   else if (opt_int(arg, "LINESIZE=", &txtfile_linesize, 4, MAXLINE));
   else {
      fatal("bad option: %s\n\n", option); }
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


void output_chars(void) {
   for (int i = 0; i < (txtfile_linesize - linecnt); ++i) fprintf(txtf, "  "); // space out to character area
   fprintf(txtf, "  "); // decorate?
   for (int i = 0; i < linecnt; ++i) output_char(buffer[i]); };

int main(int argc, char *argv[]) {
   if (argc == 1) {
      SayUsage();
      exit(4); }
   int fn = HandleOptions(argc, argv);
   if (fn == 0) fatal("no filename given");
   char *basename = argv[fn];
   char filename[MAXLINE];
   snprintf(filename, MAXLINE, "%s.tap", basename);
   if ((inf = fopen(filename, "rb")) == NULL)
      fatal("can't open \"%s\"", filename);
   printf(" opened %s\n", filename);
   if (txtfile_chartype == NOCHAR && txtfile_numtype == NONUM) txtfile_chartype = ASC;
   txtfile_doboth = txtfile_chartype != NOCHAR && txtfile_numtype != NONUM;
   if (txtfile_linesize == 0) txtfile_linesize = txtfile_doboth ? 40 : 80;
   snprintf(filename, MAXLINE, "%s.%s%s%s.txt", basename,
            numtype_options[txtfile_numtype] + 1,
            txtfile_doboth ? "." : "",
            chartype_options[txtfile_chartype] + 1);
   if ((txtf = fopen(filename, "w")) == NULL)
      fatal("can't create \"%s\"", filename);
   printf("created %s\n", filename);
   fprintf(txtf, "dumptap file:%s\n", argv[fn]);
   fprintf(txtf, "options: %s %s -LINESIZE=%d\n",
           numtype_options[txtfile_numtype], chartype_options[txtfile_chartype], txtfile_linesize);
   while (1) {
      uint32_t marker, length;
      marker = get_marker();
      if (marker == 0xffffffffL) {
         fprintf(txtf, ".tap end of medium\n");
         break; }
      if (marker == 0xfffffffeL) fprintf(txtf, ".tap erase gap\n");
      if (marker == 0x00000000L) fprintf(txtf, ".tap tape mark\n");
      else { // data record
         if (marker & 0x7f000000L) fatal(".tap bad marker: %08lX", marker);
         if ((length = marker & 0xffffffL) == 0) fatal(".tap bad record length: %08lX", marker);
         fprintf(txtf, "%c%4d: ", marker & 0x80000000L ? '!' : ' ', length); // show ! if the block had errors
         linecnt = 0;
         for (unsigned int i = 0; i < length; ++i) { // for all bytes in the data record
            byte ch = readbyte();
            if (linecnt >= txtfile_linesize) { // start a new line
               if (txtfile_doboth) output_chars();
               fprintf(txtf, "\n       ");
               linecnt = 0; }
            buffer[linecnt++] = ch;
            if (txtfile_numtype == HEX) fprintf(txtf, "%02X", ch);
            else if (txtfile_numtype == OCT) fprintf(txtf, "%02o", ch & 0x3f);
            else output_char(ch); }
         if (txtfile_doboth) output_chars();
         fprintf(txtf, "\n");
         if (length & 1) readbyte(); // data is padded to an even number of bytes
         marker = get_marker();
         if ((marker & 0xffffffL) != length) fatal("bad ending marker: %08lX", marker); } }
   fclose(inf);
   fclose(txtf);
   return 0; }

//*
