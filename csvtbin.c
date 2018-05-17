//file: csvtbin.c
/******************************************************************************

Convert a Saleae .csv file with digitized samples of analog tape head 
voltages to/from a binary .tbin file with roughly the same data,
perhaps with some loss of precision.

Why? Because we get about a 10:1 reduction in the file size
(using 16-bit non-delta samples), and it's faster to process.

The file format (defined in csvtbin.h) allows for an arbitrary number
of blocks of data with arbitrary number of bits per sample, which could
be deltas from the previous sample.  But at the moment this code only
implements one block of 16-bit non-delta samples.

Although not required, it is strongly suggested that the track order
be specified if it is not the standard MSB...LSB,PARITY.
By doing so the .tbin files will be in the canonical order, and
the readtape program won't need to be told about the non-standard order.

*******************************************************************************

---CHANGE LOG ---

*** 10 May 2018, L. Shustek, started with inspiration from Al Kossow

******************************************************************************/
#define VERSION "17May2018"
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

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
typedef unsigned char byte;

#include "csvtbin.h"

#define MAXVOLTS 8.0   // maximum voltage; could be determined dynamically
#define MAXLINE 500
#define MAXPATH 300
#define MAXTRKS 9

FILE *inf, *outf;
long long int num_samples = 0;
uint64_t total_time = 0;
int skip_samples = 0;
int ntrks = 9;
bool wrote_hdr = false;
bool do_read = false;
bool little_endian;
int track_permutation[MAXTRKS] = { -1 };
float samples[MAXTRKS];
struct tbin_hdr_t hdr = { HDR_TAG };
struct tbin_dat_t dat = { DAT_TAG };

static void vfatal(const char *msg, va_list args) {
   printf("\n***FATAL ERROR: ");
   vprintf(msg, args);
   printf("\n");
   exit(99); }

void assert(bool t, const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   if (!t) vfatal(msg, args);
   va_end(args); }

void fatal(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   vfatal(msg, args);
   va_end(args); }

/********************************************************************
Routines for processing options
*********************************************************************/
void SayUsage(void) {
   static char *usage[] = {
      "use: csvtbin <options> <infilename> <outfilename>",
      "options:",
      "  -ntrks=n  the number of tracks; default is 9",
      "  -order=   input data order for bits 0..ntrks-2 and P, where 0=MSB",
      "            default is 01234567P for 9 trks, 012345P for 7 trks",
      "  -skip=n   skip the first n samples",
      "  -read     read tbin and create csv (otherwise, the opposite)",
      "optional documentation that can be recorded in the TBIN file:",
      "  -descr=txt            what is on the tape",
      "  -pe                   PE enecoding",
      "  -nrzi                 NRZI encding",
      "  -gcr                  GCR enoding",
      "  -ips=n                speed in inches/sec",
      "  -bpi=n                density in bits/inch",
      "  -datewritten=ddmmyyyy when the tape was originally written",
      "  -dateread=ddmmyyyy    when the tape was read and digitized",
      NULL };
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]); }

bool opt_key(const char* arg, const char* keyword) {
   do { // check for a keyword option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   return *arg == '\0'; }

bool opt_int(const char* arg, const char* keyword, int *pval, int min, int max) {
   do { // check for a "keyword=integer" option
      if (toupper(*arg++) != *keyword++)
         return false; }
   while (*keyword);
   int num, nch;
   if (sscanf(arg, "%d%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') return false;
   *pval = num;
   return true; }

bool opt_flt(const char* arg, const char* keyword, float *pval, float min, float max) {
   do { // check for a "keyword=float" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   float num;  int nch;
   if (sscanf(arg, "%f%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') return false;
   *pval = num;
   return true; }

bool opt_str(const char* arg, const char* keyword, const char** str) {
   do { // check for a "keyword=string" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   *str = arg; // ptr to "string" part, which could be null
   return true; }

bool parse_nn(const char *str, int *num, int low, int high) {
   if (!isdigit(*str) || !isdigit(*(str + 1))) return false;
   *num = (*str - '0') * 10 + (*(str + 1) - '0');
   return *num >= low && *num <= high; }

bool opt_dat(const char* arg, const char*keyword, struct tm *time) {
   do { // check for a "keyword=ddmmyyyy" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   if (strlen(arg) != 8) return false;
   if (!parse_nn(arg, &time->tm_mday, 1, 31)) return false;
   if (!parse_nn(arg + 2, &time->tm_mon, 1, 12)) return false;
   --time->tm_mon;
   int yyh, yyl;
   if (!parse_nn(arg + 4, &yyh, 19, 21)) return false;
   if (!parse_nn(arg + 6, &yyl, 0, 99)) return false;
   time->tm_year = (yyh - 19) * 100 + yyl;
   return true; }

bool parse_track_order(const char*str) { // examples: P314520, 01234567P
   int bits_done = 0;
   if (strlen(str) != ntrks) return false;
   for (int i = 0; i < ntrks; ++i) {
      byte ch = str[i];
      if (toupper(ch) == 'P') ch = ntrks - 1; // we put parity last
      else {
         if (!isdigit(ch)) return false; // assumes ntrks <= 11
         if ((ch -= '0') > ntrks - 2) return false; }
      track_permutation[i] = ch;
      bits_done |= 1 << ch; }
   return bits_done + 1 == (1 << ntrks); } // must be a permutation of 0..ntrks-1

bool parse_option(char *option) {
   if (option[0] != '/' && option[0] != '-') return false;
   char *arg = option + 1;
   char *str;
   if (opt_key(arg, "READ")) do_read = true;
   else if (opt_int(arg, "NTRKS=", &ntrks, 5, 9))
      assert(track_permutation[0] == -1, "can't give -ntrks after -order");
   else if (opt_str(arg, "ORDER=", &str)
            && parse_track_order(str));
   else if (opt_key(arg, "NRZI")) hdr.u.s.mode = NRZI;
   else if (opt_key(arg, "PE")) hdr.u.s.mode = PE;
   else if (opt_key(arg, "GCR")) hdr.u.s.mode = GCR;
   else if (opt_flt(arg, "BPI=", &hdr.u.s.bpi, 50, 10000));
   else if (opt_flt(arg, "IPS=", &hdr.u.s.ips, 10, 100));
   else if (opt_str(arg, "DESCR=", &str)) {
      strncpy(hdr.descr, str, sizeof(hdr.descr));
      hdr.descr[sizeof(hdr.descr) - 1] = '\0'; }
   else if (opt_dat(arg, "DATEWRITTEN=", &hdr.u.s.time_written)) ;
   else if (opt_dat(arg, "DATEREAD=", &hdr.u.s.time_read)) ;
   else if (opt_int(arg, "SKIP=", &skip_samples, 0, INT_MAX)) {
      printf("Will skip the first %d samples\n", skip_samples); }
   else if (option[2] == '\0') // single-character switches
      switch (toupper(option[1])) {
      case 'H':
      case '?': SayUsage(); exit(1);
      default: goto opterror; }
   else {
opterror:  fatal("bad option: %s\n\n", option);
      // SayUsage();
      exit(4); }
   return true; }

int HandleOptions(int argc, char *argv[]) {
   /* returns the index of the first argument that is not an option;
   i.e. does not start with a dash or a slash */
   int i, firstnonoption = 0;
   //for (i = 0; i < argc; ++i) printf("arg %d: \"%s\"\n", i, argv[i]);
   for (i = 1; i < argc; i++) {
      if (!parse_option(argv[i])) { // end of switches
         firstnonoption = i;
         break; } }
   return firstnonoption; }

/********************************************************************
Fast routines for processing tape data
*********************************************************************/
float scanfast_float(char **p) { // *** fast scanning routines for the CSV numbers in the input file
   float n = 0;
   bool negative = false;
   while (**p == ' ' || **p == ',')++*p; //skip leading blanks, comma
   if (**p == '-') { // optional minus sign
      ++*p;
      negative = true; }
   while (isdigit(**p)) n = n * 10 + (*(*p)++ - '0'); //accumulate left of decimal point
   if (**p == '.') { // skip decimal point
      float divisor = 10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ - '0') / divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

double scanfast_double(char **p) {
   double n = 0;
   bool negative = false;
   while (**p == ' ' || **p == ',')++*p;  //skip leading blanks, comma
   if (**p == '-') { // optional minus sign
      ++*p;
      negative = true; }
   while (isdigit(**p)) n = n * 10 + (*(*p)++ - '0'); //accumulate left of decimal point
   if (**p == '.') {
      double divisor = 10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ - '0') / divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

// While we're at it: Microsoft Visual Studio C doesn't support the wonderful POSIX %' format
// specifier for nicely displaying big numbers with commas separating thousands, millions, etc.
// So here are a couple of special-purpose routines for that.
// *** BEWARE *** THEY USE A STATIC BUFFER, SO YOU CAN ONLY DO ONE CALL PER LINE!

char *intcommas(int n) { // 32 bits
   assert(n >= 0, "bad call to intcommas: %d", n);
   static char buf[14]; //max: 2,147,483,647
   char *p = buf + 13;  int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ','; ctr = 3; }
         *p-- = n % 10 + '0';
         n = n / 10; }
   return p + 1; }

char *longlongcommas(long long n) { // 64 bits
   assert(n >= 0, "bad call to longlongcommas: %ld", n);
   static char buf[26]; //max: 9,223,372,036,854,775,807
   char *p = buf + 25; int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ',';  ctr = 3; }
         *p-- = n % 10 + '0';
         n = n / 10; }
   return p + 1; }

/********************************************************************
Fast routines for processing the files
*********************************************************************/
void output2(uint16_t num) { //output a 2-byte little-endian quantity
   for (int i = 0; i < 2; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output2");
      num >>= 8; } }

void output4(uint32_t num) {  //output a 4-byte little-endian quantity
   for (int i = 0; i < 4; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output4");
      num >>= 8; } }

void output8(uint64_t num) {  //output an 8-byte little-endian quantity
   for (int i = 0; i < 8; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output8");
      num >>= 8; } }

void reverse2(uint16_t *pnum) {
   byte x = ((byte *)pnum)[0];
   byte y = ((byte *)pnum)[1];
   ((byte *)pnum)[0] = y;
   ((byte *)pnum)[1] = x; }

void reverse4(uint32_t *pnum) {
   for (int i = 0; i < 2; ++i) {
      byte x = ((byte *)pnum)[i];
      byte y = ((byte *)pnum)[3-i];
      ((byte *)pnum)[i] = y;
      ((byte *)pnum)[3-i] = x; } }

void reverse8(uint64_t *pnum) {
   for (int i = 0; i < 4; ++i) {
      byte x = ((byte *)pnum)[i];
      byte y = ((byte *)pnum)[7 - i];
      ((byte *)pnum)[i] = y;
      ((byte *)pnum)[7 - i] = x; } }

void show_tm(struct tm *t, const char*msg) {
   printf("%s: %d/%d/%d, ", msg, t->tm_mon + 1, t->tm_mday, t->tm_year + 1900);
   printf("%dh:%dm:%ds\n", t->tm_hour, t->tm_min, t->tm_sec); }

char *modename(enum mode_t mode) {
   return mode == PE ? "PE" : mode == NRZI ? "NRZI" : mode == GCR ? "GCR" : "???"; }

void read_tbin(void) {
   assert(fread(&hdr, sizeof(hdr), 1, inf) == 1, "can't read hdr");
   assert(strcmp(hdr.tag, HDR_TAG) == 0, "bad hdr tag");
   if (!little_endian)  // convert all 4-byte integers in the header to big-endian
      for (int i = 0; i < sizeof(hdr.u.s) / 4; ++i)
         reverse4(&hdr.u.a[i]);
   assert(hdr.u.s.format == TBIN_FILE_FORMAT, "bad file format version: %d", hdr.u.s.format);
   assert(hdr.u.s.tbinhdrsize == sizeof(hdr), "bad hdr size: %d, not %d", hdr.u.s.tbinhdrsize, sizeof(hdr));
   printf("file format %d, ntrks %d, encoding %s, max %.2fV, bpi %.2f, ips %.2f, sample delta %.2f uS\n",
          hdr.u.s.format, hdr.u.s.ntrks, modename(hdr.u.s.mode), hdr.u.s.maxvolts,
          hdr.u.s.bpi, hdr.u.s.ips, (float)hdr.u.s.tdelta/1e3);
   printf("description: %s\n", hdr.descr);
   if (hdr.u.s.time_written.tm_year > 0)   printf("created on:   %s", asctime(&hdr.u.s.time_written));
   if (hdr.u.s.time_read.tm_year > 0)      printf("read on:      %s", asctime(&hdr.u.s.time_read));
   if (hdr.u.s.time_converted.tm_year > 0) printf("converted on: %s", asctime(&hdr.u.s.time_converted));
   assert(fread(&dat, sizeof(dat), 1, inf) == 1, "can't read dat");
   assert(strcmp(dat.tag, DAT_TAG) == 0, "bad dat tag");
   if (!little_endian) reverse8(&dat.tstart); // convert to big endian if necessary
   printf("%d bits/sample, data start time is %.6lf seconds\n", dat.sample_bits, (double)dat.tstart / 1e9);
   assert(dat.sample_bits == 16, "Sorry, we only support 16-bit voltage samples");
   fprintf(outf, "'%s\nTime, ", hdr.descr); // first line is description, second is column headings
   for (int i = 0; i < ntrks; ++i) fprintf(outf, "Track %d, ", i);
   fprintf(outf, "\n");
   uint64_t timenow = dat.tstart;
   int16_t data[MAXTRKS];
   if (skip_samples > 0) {
      printf("skipping %d samples\n", skip_samples);
      while (skip_samples--) {
         assert(fread(data, 2, ntrks, inf) == ntrks, "endfile with %d samples left to skip", skip_samples);
         timenow += hdr.u.s.tdelta; } }
   while (1) {  // write one .CSV file line for each sample we read
      assert(fread(&data[0], 2, 1, inf) == 1, "can't read data for track 0 at time %.8lf", (double)timenow/1e9);
      if (!little_endian) reverse2(&data[0]);
      if (data[0] == (int16_t)0x8000) return;
      assert(fread(&data[1], 2, ntrks-1, inf) == ntrks-1, "can't read data for tracks 1.. at time %.8lf", (double)timenow/1e9);
      if (!little_endian)
         for (int trk = 1; trk < ntrks; ++trk)
            reverse2(&data[trk]);
      fprintf(outf, "%12.8lf, ", (double)timenow/1e9);
      timenow += hdr.u.s.tdelta;
      total_time += hdr.u.s.tdelta;
      for (int trk = 0; trk < ntrks; ++trk)
         fprintf(outf, "%9.5f, ", (float)data[track_permutation[trk]] / 32767 * hdr.u.s.maxvolts);
      fprintf(outf,"\n");
      ++num_samples;
      if (num_samples % 100000 == 0) printf("."); // progress indicator
   } };

void write_hdr(uint64_t start, uint32_t delta) {
   // create and write the ID block
   hdr.u.s.tbinhdrsize = sizeof(hdr);
   hdr.u.s.format = TBIN_FILE_FORMAT;
   time_t start_time;
   time(&start_time);
   struct tm *ptm = localtime(&start_time);
   hdr.u.s.time_converted = *ptm;  // structure copy!
   hdr.u.s.ntrks = ntrks;
   hdr.u.s.tdelta = delta;
   hdr.u.s.maxvolts = MAXVOLTS;
   assert(fwrite(hdr.tag, sizeof(hdr.tag)+sizeof(hdr.descr), 1, outf) == 1, "can't write hdr tag/descr");
   for (int i = 0; i < sizeof(hdr.u.s) / 4; ++i)
      output4(hdr.u.a[i]);  // write all the 4-byte quantities as little-endian
   // Create and write the data header. Eventually there could be more than one of these.
   dat.tstart = start;
   dat.sample_bits = 16;  // the only thing we support right now
   assert(fwrite(dat.tag, sizeof(dat)-sizeof(dat.tstart), 1, outf) == 1, "can't write dat tag");
   output8(dat.tstart); }

void write_tbin(void) {
   char line[MAXLINE + 1];
   char *linep;
   fgets(line, MAXLINE, inf); // first two (why?) lines in the input file are headers from Saleae
   fgets(line, MAXLINE, inf);
   if (skip_samples > 0) {
      printf("skipping %d samples\n", skip_samples);
      while (skip_samples--)
         assert(fgets(line, MAXLINE, inf), "endfile with %d lines left to skip\n", skip_samples); }
   line[MAXLINE - 1] = 0;
   assert(fgets(line, MAXLINE, inf), "endfile reading first data line\n");
   linep = line;  // read and discard the first line, to get the starting time so we can compute delta
   uint64_t starting_time = (uint64_t) (scanfast_double(&linep)*1e9);
   while (fgets(line, MAXLINE, inf)) {
      linep = line;
      uint64_t sample_time;
      float fsample, round;
      int32_t sample;
      sample_time = (uint64_t) (scanfast_double(&linep)*1e9);
      if (!wrote_hdr) { // write the header, once, at the second data line
         write_hdr(sample_time, (uint32_t) (sample_time - starting_time));
         wrote_hdr = true; }
      //printf("%4lld: ", num_samples);
      for (int trk = 0; trk < ntrks; ++trk)  // read and permute the samples
         samples[track_permutation[trk]] = scanfast_float(&linep);
      byte outbuf[MAXTRKS * 2]; // accumulate data for all track, for faster writing
      int bufndx = 0;
      for (int trk = 0; trk < ntrks; ++trk) { // generate the little-endian integer samples
         fsample = samples[trk];
         if (fsample < 0) round = -0.5;  else round = 0.5;  // (int) truncates towards zero
         sample = (int) ((fsample / MAXVOLTS * 32767) + round);
         //printf("%6d %9.5f, ", sample, fsample);
         assert(sample <= 32767, "sample %lld on track %d is too big: %d", num_samples, trk, sample);
         assert(sample >= -32767, "sample %lld on track %d is too small: %d", num_samples, trk, sample);
         outbuf[bufndx++] = ((int16_t)sample) & 0xff;
         outbuf[bufndx++] = ((int16_t)sample) >> 8; }
      //printf("\n");
      assert(fwrite(outbuf, 2, ntrks, outf) == ntrks, "can't write data sample %lld", num_samples);
      total_time += hdr.u.s.tdelta;
      ++num_samples;
      if (num_samples % 100000 == 0) printf("."); // progress indicator
   }
   output2(0x8000); // end marker
   };

void main(int argc, char *argv[]) {
   int argnext;
   char *filename;
#if 0
#define showsize(x) printf(#x " is %d bytes\n", (int)sizeof(x));
   showsize(hdr.u.s.mode);
   showsize(hdr.u.s);
   showsize(hdr.u);
   showsize(hdr);
   showsize(dat);
   printf("%d arguments:\n", argc);
   for (int i = 0; i < argc; ++i) printf(" arg %d: %s\n", i, argv[i]);
#endif
  
   printf("csvtbin: convert between .CSV and .TBIN files\n");
   printf("         version \"%s\" was compiled on %s at %s\n", VERSION, __DATE__, __TIME__);
   uint32_t testendian = 1;
   little_endian = *(byte *)&testendian == 1;
   printf("         this is a %s-endian computer\n", little_endian ? "little" : "big");
   if (argc == 1) {
      SayUsage(); exit(4); }
   argnext = HandleOptions(argc, argv);

   assert(argnext > 0, "missing input filename");
   filename = argv[argnext];
   printf("opening  \"%s\"\n", filename);
   inf = fopen(filename, do_read ? "rb" : "r");
   assert(inf, "unable to open input file\"%s\"", filename);

   assert(++argnext < argc, "missing output filename");
   filename = argv[argnext];
   printf("creating \"%s\"\n", filename);
   outf = fopen(filename, do_read ? "w" : "wb");
   assert(outf, "file create failed for \"%s\"", filename);

   assert(++argnext == argc, "extra stuff \"%s\"", argv[argnext]);

   if (track_permutation[0] == -1) // no input value permutation was given
      for (int i = 0; i < ntrks; ++i) track_permutation[i] = i; // create default
   printf("%s track order: ", do_read ? "output" : "input");
   for (int i = 0; i < ntrks; ++i)
      if (track_permutation[i] == ntrks - 1) printf("p");
      else printf("%d", track_permutation[i]);
   printf("\n");

   time_t start_time = time(NULL);
   if (do_read) read_tbin();
   else write_tbin();

   printf("\n%s samples representing %.3lf tape seconds were processed in %.1f seconds\n",
      longlongcommas(num_samples), (float)total_time / 1e9, difftime(time(NULL), start_time));

   fclose(inf);
   fclose(outf); };

//*