//file: csvtbin.c
/******************************************************************************

Convert between a Saleae .csv file with digitized samples of analog tape
head voltages and a binary .tbin file with roughly the same data,
perhaps with some loss of precision.

usage:  csvtbin  <options>  <basefilename>

Why? Because we get about a 10:1 reduction in the file size (using
16-bit non-delta samples), and it's 3-4 times faster to process.

Also, this allows us to preserve information about the tape inside the file,
and to change the track order from however the logic analyzer was wired
to a standard canonical order.

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
10 May 2018, L. Shustek, started with inspiration from Al Kossow

18 may 2018, L. Shustek, cleanup for more stringent compilers

17 Sep 2018, L. Shustek, add display tbin header mode

07 Oct 2018, L. Shustek, set sample_deltat by reading the first 10,000 samples,
             because Saleae timestamps are only given to 0.1 usec, so you can't
             compute the delta from the first two! We then ignore their
             timestamps and accumulate deltas from the first entry,
             using 64-bit integer arithmetic so there is no cumulative error.
             While we're at it, we set maxvolts based on the maximum voltage
             we saw, and add a -maxvolts= option to override it.

13 Oct 2018, L. Shustek, allow dates and numbers to be null (and ignored)
             to simplify batch files.

02 Nov 2018, L. Shustek, add -redo option; warn about data/tracks mismatch.

07 Nov 2018, L. Shustek, warn and set TBIN_NO_ORDER if tracks haven't been
             reordered. That lets readtape know when to apply the ordering
             given to it, so the same batch file can be use for .CSV and .TBIN.

17 Mar 2019, L. Shustek, add -scale option.

08 Dec 2019, L. Shustek
             Add Whirlwind, and -subsample option
V1.7         Add support for the trkorder header extension for Whirlwind.
             Add -invert and -reverse options.
             Switch to providing only <basefilename>, like for readtape.

28 Feb 2020, L. Shustek
V1.8         Allow IPS up to 200.

2 Jun 2022,  L. Shustek
V1.9         Add -graph option.
             Create a <basefilename>.csvtbin.log file.

20 Jun 2022, L. Shustek
V1.10        Add -stopaft, -starttime, -endtime options to truncate files.
             Add -stagger option to create CSV files for graphing.
             Fix -read, broken since V1.7.
             Remove use of max() macro.

22 Jul 2022, L. Shustek
V1.11        Don't generated the trailing comma on the CSV header line for -read, 
             because it makes readtape think there is an extra track

--- FUTURE VERSION IDEAS ---

- round up the auto-determined maxvolts even more, to reduce the number of
  times a redo is necessary
- display % done along with sample progress count. (Requires a system-
  independent way to find out the size of the file and how far we're read.)

******************************************************************************/
#define VERSION "1.11"
/******************************************************************************
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

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>
typedef unsigned char byte;

#include "csvtbin.h"

#define MAXPATH 300
#define MAXLINE 400
#define MINTRKS 5
#define PREREAD_COUNT 1000000

FILE *inf, *outf, *graphf, *logf;
char *basefilename;
char infilename[MAXPATH], outfilename[MAXPATH], graphfilename[MAXPATH], logfilename[MAXPATH];
uint64_t num_samples = 0;
uint64_t total_time = 0;
uint64_t skip_samples = 0, stopaft = UINT64_MAX;
float fstarttime, fendtime; // starting and ending times in floating seconds
uint64_t starttime = 0, endtime = UINT64_MAX;  // starting and ending times in nanoseconds
unsigned subsample = 1;
unsigned ntrks = 9;
unsigned int num_graph_vals = 0, graphbin = 0;
float graphbin_max = 0, stagger = 0;
bool do_read = false, display_header = false, redo = false, redid = false;
bool little_endian;
unsigned track_permutation[MAXTRKS] = { UINT_MAX };
float scalefactor = 1.0f;
float samples[MAXTRKS];
struct tbin_hdr_t hdr = { HDR_TAG };
struct tbin_hdrext_trkorder_t hdrext_trkorder = { HDR_TRKORDER_TAG };
struct tbin_dat_t dat = { DAT_TAG };

void logprintf(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   va_list args2;
   va_copy(args2, args);
   vfprintf(stdout, msg, args);  // to the console
   if (logf) vfprintf(logf, msg, args2); // and maybe also the log file
   va_end(args2); }

void vfatal(const char *msg, va_list args) {
   logprintf("\n***FATAL ERROR: ");
   vprintf(msg, args);
   if (logf) vfprintf(logf, msg, args);
   logprintf("\n");
   exit(99); }

void assert(bool t, const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   if (!t) vfatal(msg, args);
   va_end(args); }

void fatal(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   vfatal(msg, args); }

/********************************************************************
Routines for processing options
*********************************************************************/
void SayUsage(void) {
   static char *usage[] = {
      "use: csvtbin <options> <basefilename>",
      "options:",
      "  -ntrks=n      the number of tracks; the default is 9",
      "  -order=       input data order for bits 0..ntrks-2 and P, where 0=MSB",
      "                the default is 01234567P for 9 trks, 012345P for 7 trks",
      "                (for Whirlwind: a combination of C L M c l m and x's)",
      "  -skip=n       skip the first n samples",
      "  -subsample=n  use only every nth data sample",
      "  -stopaft=n    stop after doing n samples",
      "  -starttime=x  start only after sample time x",
      "  -endtime=x    end after sample time x",
      "  -invert       invert the data so positive peaks are negative and vice versa",
      "  -scale=n      scale the voltages by n, which can be a fraction",
      "  -maxvolts=x   expect x as the maximum plus or minus voltage",
      "  -redo         do it over again if maxvolts wasn't big enough",
      "  -graph=n      create a <basefilename>.graph.csv file with the maximum voltage every n samples",
      "  -read         read tbin and create csv; otherwise the opposite",
      "  -stagger=x    if -read, stagger each track by x volts for graphing",
      "  -showheader   just show the header info of a .tbin file, and check the data",
      "optional documentation that can be recorded in the TBIN file:",
      "  -descr=txt             a description of what is on the tape",
      "  -pe                    PE encoded",
      "  -nrzi                  NRZI encoded",
      "  -gcr                   GCR ecoded",
      "  -whirlwind             Whirlwind I encoded",
      "  -reverse               the tape might have been read or written backwards; mark it so",
      "  -ips=n                 the speed in inches/sec",
      "  -bpi=n                 the density in bits/inch",
      "  -datewritten=ddmmyyyy  when the tape was originally written",
      "  -dateread=ddmmyyyy     when the tape was read and digitized",
      NULL };
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]); }

char *longlongcommas(uint64_t n);

bool opt_key(const char* arg, const char* keyword) {
   do { // check for a keyword option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   return *arg == '\0'; }

bool opt_int(const char* arg, const char* keyword, unsigned *pval, unsigned min, unsigned max) {
   do { // check for a "keyword=integer" option
      if (toupper(*arg++) != *keyword++)
         return false; }
   while (*keyword);
   if (strlen(arg) == 0) return true;  // allow and ignore if null
   unsigned num, nch;
   if (sscanf(arg, "%u%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') fatal("bad integer: %s", arg);
   *pval = num;
   return true; }

bool opt_64int(const char* arg, const char* keyword, uint64_t *pval, uint64_t min, uint64_t max) {
   do { // check for a "keyword=longlonginteger" option
      if (toupper(*arg++) != *keyword++)
         return false; }
   while (*keyword);
   if (strlen(arg) == 0) return true;  // allow and ignore if null
   uint64_t num;
   unsigned nch;
   if (sscanf(arg, "%llu%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') fatal("bad integer: %s", arg);
   *pval = num;
   return true; }

bool opt_flt(const char* arg, const char* keyword, float *pval, float min, float max) {
   do { // check for a "keyword=float" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   if (strlen(arg) == 0) return true;  // allow and ignore if null
   float num;  int nch;
   if (sscanf(arg, "%f%n", &num, &nch) != 1
         || num < min || num > max || arg[nch] != '\0') fatal("bad floating-point number: %s", arg);
   *pval = num;
   return true; }

bool opt_str(const char* arg, const char* keyword, const char** str) {
   do { // check for a "keyword=string" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   *str = arg; // ptr to "string" part, which could be null
   return true; }

bool parse_nn(const char *str, int *num, int low, int high) {
   // parse a 2-digit decimal number within specified limits
   if (!isdigit(*str) || !isdigit(*(str + 1))) return false;
   *num = (*str - '0') * 10 + (*(str + 1) - '0');
   return *num >= low && *num <= high; }

bool opt_dat(const char* arg, const char*keyword, struct tm *time) {
   do { // check for a "keyword=ddmmyyyy" option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   if (strlen(arg) == 0) return true;  // allow and ignore if null
   if (strlen(arg) != 8) fatal("bad date format at %s", arg);
   if (!parse_nn(arg, &time->tm_mday, 1, 31)) fatal("bad day: %s", arg);
   if (!parse_nn(arg + 2, &time->tm_mon, 1, 12)) fatal("bad month: %s", arg);
   --time->tm_mon; // month is 0-origin!
   int yyh, yyl;
   if (!parse_nn(arg + 4, &yyh, 19, 21)) fatal("bad year: %s", arg);
   if (!parse_nn(arg + 6, &yyl, 0, 99)) fatal("bad year: %s", arg);
   time->tm_year = (yyh - 19) * 100 + yyl; // years since 1900
   return true; }

bool parse_track_order(const char*str) { // examples: P314520, 01234567P
   if (hdr.u.s.mode == WW) { // for Whirlwind, just copy the string to the header extension
      assert(strlen(str) <= MAXTRKS, "Whirlwind -order string too long:%s", str);
      strcpy(hdrext_trkorder.trkorder, str);
      ntrks = strlen(str);
      hdr.u.s.flags |= TBIN_TRKORDER_INCLUDED + TBIN_NO_REORDER;
      logprintf("using Whirlwind -order=%s and ntrks=%d\n", str, ntrks);
      return true; }
   else {// for PE, NRZI, and GCR we reorder tracks as specified; examples: P314520, 01234567P
      if (strlen(str) != ntrks) return false;
      int bits_done = 0;  // bitmap showing which tracks are done
      for (unsigned i = 0; i < ntrks; ++i) {
         byte ch = str[i];
         if (toupper(ch) == 'P') ch = (byte)ntrks - 1; // we put parity last
         else {
            if (!isdigit(ch)) return false; // assumes ntrks <= 11
            if ((ch -= '0') > ntrks - 2) return false; }
         track_permutation[i] = ch;
         bits_done |= 1 << ch; }
      return bits_done + 1 == (1 << ntrks); } } // must be a permutation of 0..ntrks-1

bool parse_option(char *option) {
   if (option[0] != '/' && option[0] != '-') return false;
   char *arg = option + 1;
   const char *str;
   if (opt_key(arg, "READ")) do_read = true;
   else if (opt_key(arg, "SHOWHEADER"))  do_read = display_header = true;
   else if (opt_int(arg, "NTRKS=", &ntrks, MINTRKS, MAXTRKS))
      assert(track_permutation[0] == UINT_MAX, "can't give -ntrks after -order");
   else if (opt_str(arg, "ORDER=", &str)) {
      if (!parse_track_order(str)) fatal("bad track order at %s", str); }
   else if (opt_key(arg, "NRZI")) hdr.u.s.mode = NRZI;
   else if (opt_key(arg, "PE")) hdr.u.s.mode = PE;
   else if (opt_key(arg, "GCR")) hdr.u.s.mode = GCR;
   else if (opt_key(arg, "WHIRLWIND")) hdr.u.s.mode = WW;
   else if (opt_key(arg, "INVERT")) hdr.u.s.flags |= TBIN_INVERTED;
   else if (opt_key(arg, "REVERSE")) hdr.u.s.flags |= TBIN_REVERSED;
   else if (opt_flt(arg, "BPI=", &hdr.u.s.bpi, 50, 10000));
   else if (opt_flt(arg, "IPS=", &hdr.u.s.ips, 10, 200));
   else if (opt_flt(arg, "MAXVOLTS=", &hdr.u.s.maxvolts, 0.1f, 15.0f));
   else if (opt_flt(arg, "SCALE=", &scalefactor, 1e-4f, 1e+4f));
   else if (opt_flt(arg, "STAGGER=", &stagger, 0.1f, 100.f));
   else if (opt_str(arg, "DESCR=", &str)) {
      strncpy(hdr.descr, str, sizeof(hdr.descr));
      hdr.descr[sizeof(hdr.descr) - 1] = '\0'; }
   else if (opt_dat(arg, "DATEWRITTEN=", &hdr.u.s.time_written)) ;
   else if (opt_dat(arg, "DATEREAD=", &hdr.u.s.time_read)) ;
   else if (opt_64int(arg, "SKIP=", &skip_samples, 0, UINT64_MAX-1)) {
      printf("will skip the first %s samples\n", longlongcommas(skip_samples)); }
   else if (opt_int(arg, "SUBSAMPLE=", &subsample, 1, INT_MAX)) {
      printf("will use every %d samples\n", subsample); }
   else if (opt_64int(arg, "STOPAFT=", &stopaft, 1, UINT64_MAX-1)) {
      printf("will stop after doing %s samples\n", longlongcommas(stopaft)); }
   else if (opt_flt(arg, "STARTTIME=", &fstarttime, 0.01f, 1000.f)) {
      starttime = (uint64_t)((double)fstarttime * 1e9); // in nanoseconds
      printf("will start at sample time %.5f\n", fstarttime); }
   else if (opt_flt(arg, "ENDTIME=", &fendtime, 0.01f, 1000.f)) {
      endtime = (uint64_t)((double)fendtime * 1e9); // in nanoseconds
      printf("will end at sample time %.5f\n", fendtime); }
   else if (opt_int(arg, "GRAPH=", &graphbin, 1, INT_MAX)) {
      printf("will record the maximum excursion every %d samples\n", graphbin); }
   else if (opt_key(arg, "REDO")) redo = true;
   else if (option[2] == '\0') // single-character switches
      switch (toupper(option[1])) {
      case 'H':
      case '?': SayUsage(); exit(1);
      default: goto opterror; }
   else {
opterror:  fatal("bad option: %s\n\n", option); }
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
   assert(starttime < endtime, "starttime is after endtime");
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

char *longlongcommas(uint64_t n) { // 64 bits
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
   logprintf("%s: %d/%d/%d, ", msg, t->tm_mon + 1, t->tm_mday, t->tm_year + 1900);
   logprintf("%dh:%dm:%ds\n", t->tm_hour, t->tm_min, t->tm_sec); }

char *modename(enum mode_t mode) {
   return mode == PE ? "PE" : mode == NRZI ? "NRZI" : mode == GCR ? "GCR" : "not specified"; }

int progress_count = 0;
void update_progress_count(void) {
   static char progress_buffer[25] = { 0 };
   if (progress_count++ >= 999999) {
      for (unsigned i = 0; i < strlen(progress_buffer); ++i) printf("\b"); // erase previous count
      sprintf(progress_buffer, "%s samples", longlongcommas(num_samples));
      printf("%s", progress_buffer);
      progress_count = 0; } }

void read_tbin(void) {
   assert(fread(&hdr, sizeof(hdr), 1, inf) == 1, "can't read hdr");
   assert(strcmp(hdr.tag, HDR_TAG) == 0, "bad hdr tag");
   if (!little_endian)  // convert all 4-byte integers in the header to big-endian
      for (int i = 0; i < sizeof(hdr.u.s) / 4; ++i)
         reverse4(&hdr.u.a[i]);
   assert(hdr.u.s.format == TBIN_FILE_FORMAT, "bad file format version: %d", hdr.u.s.format);
   assert(hdr.u.s.tbinhdrsize == sizeof(hdr), "bad hdr size: %d, not %d", hdr.u.s.tbinhdrsize, sizeof(hdr));
   ntrks = hdr.u.s.ntrks;
   logprintf("file format %d, ntrks %d, encoding %s, max %.2fV, bpi %.2f, ips %.2f, sample delta %.2f usec\n",
             hdr.u.s.format, hdr.u.s.ntrks, modename(hdr.u.s.mode), hdr.u.s.maxvolts,
             hdr.u.s.bpi, hdr.u.s.ips, (float)hdr.u.s.tdelta / 1e3);
   logprintf("the track ordering was%s given when the .tbin file was created\n", hdr.u.s.flags & TBIN_NO_REORDER ? " not" : "");
   logprintf("description: %s\n", hdr.descr);
   if (hdr.u.s.time_written.tm_year > 0)   logprintf("created on:   %s", asctime(&hdr.u.s.time_written));
   if (hdr.u.s.time_read.tm_year > 0)      logprintf("read on:      %s", asctime(&hdr.u.s.time_read));
   if (hdr.u.s.time_converted.tm_year > 0) logprintf("converted on: %s", asctime(&hdr.u.s.time_converted));
   if (hdr.u.s.flags & TBIN_INVERTED) logprintf("the data was inverted\n");
   if (hdr.u.s.flags & TBIN_REVERSED) logprintf("the tape might have been read or written backwards\n");
   if (hdr.u.s.flags & TBIN_TRKORDER_INCLUDED) { // optional trkorder heaader extension?
      assert(fread(&hdrext_trkorder, sizeof(hdrext_trkorder), 1, inf) == 1, "can't read trkorder header extension");
      assert(strcmp(hdrext_trkorder.tag, HDR_TRKORDER_TAG) == 0, "had trkorder header extension tag");
      assert(hdr.u.s.mode == WW, "trkorder header extension included with non-Whirlwind file");
      logprintf("the Whirlwind tracks were specified as -order=%s\n", hdrext_trkorder.trkorder); }
   assert(fread(&dat, sizeof(dat), 1, inf) == 1, "can't read dat");
   assert(strcmp(dat.tag, DAT_TAG) == 0, "bad dat tag");
   if (!little_endian) reverse8(&dat.tstart); // convert to big endian if necessary
   logprintf("%d bits/sample, data start time is %.6lf seconds\n", dat.sample_bits, (double)dat.tstart / 1e9);
   assert(dat.sample_bits == 16, "Sorry, we only support 16-bit voltage samples");
   if (!display_header) {
      fprintf(outf, "'%s\nTime, ", hdr.descr); // first line is description, second is column headings
      for (unsigned i = 0; i < ntrks; ++i) fprintf(outf, "Track %d%s", i, i == ntrks-1 ? "" : ", ");
      fprintf(outf, "\n"); }
   uint64_t timenow = dat.tstart;
   int16_t data[MAXTRKS];

   if (skip_samples > 0 || starttime > 0) {
      logprintf("skipping %d-track samples\n", ntrks);
      uint64_t skipped = 0;
      do {
         assert(fread(data, 2, ntrks, inf) == ntrks, "endfile with samples left to skip");
         timenow += hdr.u.s.tdelta;
         ++skipped;
         if (skip_samples > 0) --skip_samples; }
      while (timenow < starttime || skip_samples > 0);
      logprintf("skipped %s samples\n", longlongcommas(skipped)); }

   while (1) {  // write one .CSV file line for each sample we read
      assert(fread(&data[0], 2, 1, inf) == 1, "can't read data for track 0 at time %.8lf", (double)timenow / 1e9);
      if (!little_endian) reverse2((uint16_t *)&data[0]);
      if (data[0] == -32768 /*0x8000*/) break; // endfile
      assert(fread(&data[1], 2, ntrks - 1, inf) == ntrks - 1, "can't read data for tracks 1.. at time %.8lf, data[0]=%08X",
             (double)timenow / 1e9, data[0]);
      if (!little_endian)
         for (unsigned trk = 1; trk < ntrks; ++trk)
            reverse2((uint16_t *)&data[trk]);
      // The %f floating-point display formatting is quite slow. If the -read option is ever used for production, we
      // should write fast special-purpose routines, as we did for parsing floating-point input. Could be 10x faster!
      if (!display_header) fprintf(outf, "%12.8lf, ", (double)timenow / 1e9);
      timenow += hdr.u.s.tdelta;
      total_time += hdr.u.s.tdelta;
      if (!display_header) {
         float staggeramount = 0.f; // restart stagger for track 0
         for (unsigned trk = 0; trk < ntrks; ++trk) {
            float fsample = (float)data[track_permutation[trk]] / 32767 * hdr.u.s.maxvolts;
            if (hdr.u.s.flags & TBIN_INVERTED) fsample = -fsample;  // invert, if told to
            fsample += staggeramount;
            staggeramount += stagger;
            fprintf(outf, "%9.5f, ", fsample); }
         fprintf(outf, "\n"); }
      if (++num_samples >= stopaft) break;
      if (timenow > endtime) break;
      update_progress_count(); }
   logprintf("\n"); };

void write_tbin_hdr(void) {
   // create and write the ID block
   hdr.u.s.tbinhdrsize = sizeof(hdr);
   hdr.u.s.format = TBIN_FILE_FORMAT;
   time_t start_time;
   time(&start_time);
   struct tm *ptm = localtime(&start_time);
   hdr.u.s.time_converted = *ptm;  // structure copy!
   hdr.u.s.ntrks = ntrks;
   assert(fwrite(hdr.tag, sizeof(hdr.tag)+sizeof(hdr.descr), 1, outf) == 1, "can't write hdr tag/descr");
   for (int i = 0; i < sizeof(hdr.u.s) / 4; ++i)
      output4(hdr.u.a[i]);  // write all the 4-byte quantities as little-endian
   if (hdr.u.s.flags & TBIN_TRKORDER_INCLUDED) // include the optional trkorder header extension?
      assert(fwrite(&hdrext_trkorder, sizeof(hdrext_trkorder), 1, outf) == 1, "can't write hdr trkorder extension");
   // complete and write the data header, of which there could eventually be more than one.
   dat.sample_bits = 16;  // the only thing we support right now
   assert(fwrite(dat.tag, sizeof(dat)-sizeof(dat.tstart), 1, outf) == 1, "can't write dat tag");
   output8(dat.tstart); // write separately because of possible endian reversal
}

void csv_preread(void) {
   // preread the beginning of the CSV file for two reasons:
   // 1. to compute the sample delta accurately
   // 2. to observe the max voltages
   char line[MAXLINE + 1];
   int linecounter = 0;
   double first_timestamp = -1;
   fgets(line, MAXLINE, inf); // first two lines in the input file are headers from Saleae
   fgets(line, MAXLINE, inf);
   unsigned int numcommas = 0;
   for (int i = 0; line[i]; ++i) if (line[i] == ',') ++numcommas;
   if (numcommas != ntrks) logprintf("*** WARNING *** file has %d columns of data, but ntrks=%d\n", numcommas, ntrks);
   float maxvolts = 0;
   while (fgets(line, MAXLINE, inf) && ++linecounter < PREREAD_COUNT) {
      line[MAXLINE - 1] = 0;
      char *linep = line;
      double timestamp = scanfast_double(&linep);
      if (first_timestamp < 0) {
         first_timestamp = timestamp;
         dat.tstart = (uint64_t)((first_timestamp + 0.5e-9)*1e9); }
      else hdr.u.s.tdelta = (uint32_t)(((timestamp - first_timestamp) / (linecounter - 1) + 0.5e-9) * 1e9);
      for (unsigned trk = 0; trk < ntrks; ++trk) {
         float voltage = scanfast_float(&linep) * scalefactor;
         if (voltage < 0) voltage = -voltage;
         if (maxvolts < voltage) maxvolts = voltage; } }
   maxvolts = ((float)(int)((maxvolts+0.55f)*10.0f))/10.0f; // add 0.5V and round to nearest 0.1V
   logprintf("after %s samples, the sample delta is %.2lf usec (%u nsec), samples start at %.6lf seconds, and the rounded-up maximum voltage is %.1fV\n",
             intcommas(linecounter), (double)hdr.u.s.tdelta / 1e3, hdr.u.s.tdelta, (double)dat.tstart/1e9, maxvolts);
   if (subsample > 1) {
      dat.tstart += (subsample - 1) * hdr.u.s.tdelta;
      hdr.u.s.tdelta *= subsample;
      logprintf("for subsampling every %d samples, we adjusted the delta to %.2lf usec (%u nsec), and the sample start to %.6lf seconds\n",
                subsample, (double)hdr.u.s.tdelta / 1e3, hdr.u.s.tdelta, (double)dat.tstart / 1e9); }
   if (hdr.u.s.maxvolts == 0) // maxvolts= wasn't given
      hdr.u.s.maxvolts = maxvolts;
   else if (hdr.u.s.maxvolts < maxvolts) {
      logprintf("maxvolts was increased from %.1f to %.1f\n", hdr.u.s.maxvolts, maxvolts);
      hdr.u.s.maxvolts = maxvolts; }
   else logprintf("we used maxvolts=%.1f\n", hdr.u.s.maxvolts);
   fclose(inf); // close and reopen to reposition at the start
   assert(fopen(infilename, "r"), "can't reopen input file \"%s\"", infilename); }


void write_tbin(void) {
   char line[MAXLINE + 1];
   char *linep;
   csv_preread();    // do preread scan of csv file
   for (int tries = 0; tries < 2; ++tries) { // try up to two times
      write_tbin_hdr(); // write the tbin headers
      fgets(line, MAXLINE, inf); // first two lines in the input file are headers from Saleae
      fgets(line, MAXLINE, inf);
      uint64_t sample_time = dat.tstart;

      if (skip_samples > 0 || starttime > 0) {
         //logprintf("skipping samples\n");
         uint64_t skipped = 0;
         do {
            assert(fgets(line, MAXLINE, inf), "endfile with samples left to skip\n");
            sample_time += hdr.u.s.tdelta;
            ++skipped;
            if (skip_samples > 0) --skip_samples; }
         while (sample_time < starttime || skip_samples > 0);
         logprintf("skipped %s samples\n", longlongcommas(skipped)); }

      line[MAXLINE - 1] = 0;
      long long count_toosmall = 0, count_toobig = 0;
      float maxvolts = 0, minvolts = 0;
      while (1) {
         for (unsigned skip = 0; skip < subsample; ++skip)
            if (!fgets(line, MAXLINE, inf)) goto done;
         linep = line;
         float fsample, round;
         int32_t sample;
         scanfast_double(&linep); // scan and discard the timestamp
         for (unsigned trk = 0; trk < ntrks; ++trk)  // read and permute the samples
            samples[track_permutation[trk]] = scanfast_float(&linep) * scalefactor;
         byte outbuf[MAXTRKS * 2]; // accumulate data for all tracks, for faster writing
         int bufndx = 0;
         for (unsigned trk = 0; trk < ntrks; ++trk) { // generate the little-endian integer samples
            fsample = samples[trk];
            if (hdr.u.s.flags & TBIN_INVERTED) fsample = -fsample;  // invert, if told to
            if (fsample < 0) round = -0.5;  else round = 0.5;  // (int) truncates towards zero
            sample = (int)((fsample / hdr.u.s.maxvolts * 32767) + round);
            //printf("%6d %9.5f, ", sample, fsample);
            if (fsample < minvolts) minvolts = fsample;
            if (fsample > maxvolts) maxvolts = fsample;
            if (graphbin && tries == 0) {
               float abs_sample = fsample < 0 ? -fsample : fsample;
               if (abs_sample > graphbin_max) graphbin_max = abs_sample; }
            if (sample <= -32767) {
               sample = -32767; ++count_toosmall; }
            if (sample >= 32767) {
               sample = 32767;  ++count_toobig; }
            outbuf[bufndx++] = ((int16_t)sample) & 0xff;
            outbuf[bufndx++] = ((int16_t)sample) >> 8; }
         //printf("\n");
         assert(fwrite(outbuf, 2, ntrks, outf) == ntrks, "can't write data sample %lld", num_samples);
         sample_time += hdr.u.s.tdelta;
         total_time += hdr.u.s.tdelta;
         if (++num_samples >= stopaft) break;
         if (sample_time > endtime) break;
         if (graphbin && tries == 0 && ++num_graph_vals >= graphbin) {
            fprintf(graphf, "%llu, %f\n", num_samples, graphbin_max);
            graphbin_max = 0;
            num_graph_vals = 0; }
         update_progress_count(); }
done:
      output2(0x8000); // end marker
      logprintf("\ndone; minimum voltage was %.1fV, maximum voltage was %.1fV\n", minvolts, maxvolts);
      if (count_toobig)
         logprintf("*** WARNING ***  %s samples were too big\n", longlongcommas(count_toobig));
      if (count_toosmall)
         logprintf("*** WARNING ***  %s samples were too small\n", longlongcommas(count_toosmall));
      if (count_toobig || count_toosmall) {
         float newmax = maxvolts > -minvolts ? maxvolts : -minvolts;
         if (!redo) {
            logprintf("you should specify -maxvolts=%.1f\n", newmax + 0.1);
            return; // don't do it a second time
         }
         hdr.u.s.maxvolts = ((float)(int)((newmax + 0.15)*10.0f)) / 10.0f; // add .1V and round to .1V
         logprintf("redoing the conversion with -maxvolts=%.1f\n", hdr.u.s.maxvolts);
         redid = true;
         num_samples = progress_count = 0;
         total_time = 0;
         fclose(inf); fclose(outf);
         assert(inf = fopen(infilename, "r"), "unable to reopen input file\"%s\"", infilename);
         assert(outf = fopen(outfilename, "wb"), "unable to reopen output file\"%s\"", outfilename); }
      else
         return;// all sample voltages were less than maxvolts
   } };

int main(int argc, char *argv[]) {
   int argnext;
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
   printf("version %s was compiled on %s at %s\n", VERSION, __DATE__, __TIME__);
   uint32_t testendian = 1;
   little_endian = *(byte *)&testendian == 1;
   printf("this is a %s-endian computer\n", little_endian ? "little" : "big");
   if (argc == 1) {
      SayUsage(); exit(4); }
   argnext = HandleOptions(argc, argv);

   assert(argnext > 0, "missing basefilename");
   basefilename = argv[argnext];
   assert(argnext == argc-1, "unknown stuff: %s", argv[argnext + 1]);

   strncpy(logfilename, basefilename, MAXPATH - 15); logfilename[MAXPATH - 15] = 0;
   strcat(logfilename, ".csvtbin.log");
   logf = fopen(logfilename, "w");
   assert(logf, "file create failed for %s", logfilename);
   fprintf(logf, "CSVTBIN version %s compiled on %s at %s\n", VERSION, __DATE__, __TIME__);
   logprintf("command line: ");
   for (int i = 0; i < argc; ++i)  // for documentation, show invocation options
      logprintf("%s ", argv[i]);
   logprintf("\n");

   strncpy(infilename, basefilename, MAXPATH - 10); infilename[MAXPATH - 10] = 0;
   strcat(infilename, do_read ? ".tbin": ".csv");
   logprintf("opening  %s\n", infilename);
   inf = fopen(infilename, do_read ? "rb" : "r");
   assert(inf, "unable to open input file %s", infilename);

   strncpy(outfilename, basefilename, MAXPATH - 10); outfilename[MAXPATH - 10] = 0;
   strcat(outfilename, do_read ? ".csv" : ".tbin");
   logprintf("creating %s\n", outfilename);
   outf = fopen(outfilename, do_read ? "w" : "wb");
   assert(outf, "file create failed for %s", outfilename);

   if (graphbin) {
      strncpy(graphfilename, basefilename, MAXPATH - 12); graphfilename[MAXPATH - 12] = 0;
      strcat(graphfilename, ".graph.csv");
      logprintf("creating %s\n", graphfilename);
      graphf = fopen(graphfilename, "w");
      assert(graphf, "file create failed for %s", graphfilename); }

   if (track_permutation[0] == UINT_MAX) { // no input value permutation was given
      if (!do_read && !(hdr.u.s.flags & TBIN_TRKORDER_INCLUDED)) {
         logprintf("WARNING: using the default track ordering, and marking the .tbin file to show it wasn't given\n");
         hdr.u.s.flags |= TBIN_NO_REORDER; }
      for (unsigned i = 0; i < ntrks; ++i) track_permutation[i] = i; // create default
   }
   if (!display_header) {
      logprintf("%s track order: ", do_read ? "output" : "input");
      if (hdr.u.s.flags & TBIN_TRKORDER_INCLUDED)
         logprintf(hdrext_trkorder.trkorder);
      else {
         for (unsigned i = 0; i < ntrks; ++i)
            if (track_permutation[i] == ntrks - 1) logprintf("p");
            else logprintf("%d", track_permutation[i]); }
      logprintf("\n");
      if (hdr.u.s.flags & TBIN_INVERTED) logprintf("the data will be inverted\n");
      if (hdr.u.s.flags & TBIN_REVERSED) logprintf("the tape might have been read or written backwards\n"); }

   if (scalefactor != 1.0f)
      logprintf("input voltages will be scaled by %f\n", scalefactor);

   time_t start_time = time(NULL);
   if (do_read) read_tbin();
   else write_tbin();

   logprintf("%s samples representing %.3lf tape seconds were processed in %.1f seconds\n",
             longlongcommas(num_samples), (float)total_time / 1e9, difftime(time(NULL), start_time));
   fclose(inf);
   if (outf) fclose(outf);
   if (graphf) fclose(graphf);
   return 0; };

//*