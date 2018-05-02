//file: readtape.c
/************************************************************************

Read an IBM formatted 1600 BPI 9-track labelled tape
and create one disk file for each tape file.

The input is a CSV file with 10 columns:a timestamp in seconds,
and then the read head voltage for 9 tracks in the order
MSB...LSB, parity. The first two rows are text column headings.

*************************************************************************
Copyright (C) 2018, Len Shustek

---CHANGE LOG ---

*** 20 Jan 2018, L. Shustek, Started first version

*** 05 Feb 2018, L. Shustek, First github posting, since it kinda works.

*** 09 Feb 2018, L. Shustek
- Replace sscanf for a 20x speedup!
- Made major changes to the decoding algorithm:
 - In the voltage domain, do idle detection based on peak-to-peak
   voltage, not proximity to a baseline resting level.
 - In the time domain, do clock simulation when the signal drops out.
   In other words,,we fake bits when we see no flux transitions.

*** 12 Feb 2018, L. Shustek	 Major restructure:
- Allow blocks to be processed multiple times with different sets of
  deocding parameters. We then choose the best of the bad decodings.

*** 19 Feb 2018, L. Shustek. Change to the peak detection algorithm:
- Interpolate the time of the peak when multiple samples are close to
  the peak. This significantly improves decoding for low sampling rates.
  Sampling at 781 KS/s for 1600 BPI PE tapes running at 50 IPS generates
  about 10 samples per bit, and seems to work fine now.

*** 20 Feb 2018, L. Shustek,
- Add the "move threshold" to the variable parameters, because setting it
  low for some blocks lets us correctly decode some really grungy data.

*** 26 Feb 2018, L. Shustek
- Improve the decsion rules for how many fake bit to add during a
  dropout. Should we make the strategy choice a variable parameter?
- Add Automatic Gain Control (AGC) on a track-by-track basis.
  That helps a lot in decoding dropouts without picking up all
  sorts of noise between data blocks that results from reducing
  the move threshold parameter that applies to all tracks.
  The AGC is computed using an exponential weighted average
  of recent peak-to-peak voltage measurements.
- Use ASTYLE's Python-like formatting to maximum algorithmic protein
  visible on a screen and minimize the clutter of syntatic sugar.

*** 27 Feb 2018, L. Shustek
- Add a parameter that does an exponential weighted average instead
  of a moving window average to track the clock rate, like we do
  for the AGC amplitude tracking.
- Changed to use Microsoft Visual Studio 2017 as the IDE. As much as
  I've liked lcc-win64 over the years, it has too many bugs and is no
  longer being supported by Jacob Navia.  That's shame; it was nice.
  The good news: code from VS in x64 "release" mode runs 3x faster!

*** 28 Feb 2018, L. Shustek
- Compensate for pulse shifting on tape by keeping track of how far
  off a pulse is from the expected time, and using some fraction of
  that (as specified by the parameter block) to adjust the window
  that determines if the next pulse is clock or data. This is a
  pretty big win for some funky blocks.

*** 4 Mar 2018, L. Shustek
- Fix AGC updating: do only at a new peak, not at each sample.
- Add AGC mode based on "minimum of the last n peaks" as an
  alternative to the "exponential average of the last n peaks".
  This helps because 2f dropouts are often preceeded by good f peaks.
- Add a filelist input to make regression testing easier.

*** 12 Mar 2018, L. Shustek
- Add NRZI decoding for 9-track 800 BPI tapes
- Add zero-band noise surpression, parametrized

*** 21 Mar 2018, L. Shustek
- Create separate parameter blocks for PE/NRZI/GCR
- Fix decoding of NRZI CRC/LRC bytes, which are often delayed
- NRZI: Create a new output file after each filemark
- add option to create SIMH .tap format output file
- implement AGC for NRZI

*** 22 Mar 2018, L. Shustek
- Fix introduced bugs: NRZI AGC, parm selection sequence
- Improve reporting of parameter set usage

*** 29 Mar 2018, L. Shustek
- MAJOR REWRITE of low-level peak detection code to switch from a hill-
  climbing algorithm to a moving-window shape detection algorithm.
  This gets us to 100% success with some test cases!
- Add NRZI pulse-shifting compensation based on expected transition
  times, similar to PE.
- Add AGC controls to the parameter block.
- Create new trace file lines for NRZI
- Remove bitrate ("br=") parameter and replace with bpi= and ips=.

*** 1 Apr 2018, L. Shustek
- Do interpolation for more accurate peak times when several
  samples are very close to the peak.
- Make the window width as a fraction of bit time be a parameter.
- Change the number of tracks from a compile-time constant to
  a run-time variable set by the "ntrks=" parameter.
- Collect and output NRZI transition timing statistics.
- Update reporting of parameter block statistics.

*** 8 Apr 2018, L. Shustek
- Add support for 7-track NRZI decoding, which has LRC but no CRC.
- Read parameter sets from an optional file at runtime.
  That allows tailoring them for particular problematical tape data.
- Add -order= option to allow arbitrary track ordering
- Add -even option for even vertical parity on 7-track BCD tapes.
- Don't decode tape labels when writing a SIMH .tap file.
- Don't display trailing blanks in standard label fields.

*** 12 Apr 2018, L. Shustek
- Rewrite the debugging trace routines to be deeply buffered, so
we can "change history" for events that are discovered late -- for
example, with the new moving-window algorithm, the signal peaks.
- Better display of parmsets in verbose mode.

*** 16 Apr 2018, L. Shustek
- Generalize tracing to allow multitrack voltage displays along
with the event data.

*** 1 May 2018, L. Shustek
- Add explicit optional deskewing delays when reading raw head data.
- Add "-deskew" option, which does a test read of the first block to
determine the track skew values.

*********************************************************************/
#define VERSION "1May2018"
/********************************************************************
 default bit and track numbering, where 0=msb and P=parity
            on tape       memory here    written to disk
 7-track    P012345         012345P        012345
 9-track    P37521064     01234567P      01234567

 Permutation of the "on tape" sequence to the "memory here" sequence
 is done by appropriately connecting the logic analyzer probes.
*********************************************************************
The MIT License (MIT):
Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*************************************************************************/

#include "decoder.h"

// In Windows Visual Studio 2017 fseek and ftell use 32-bit integers,
// which fails for files bigger than 2 GB!
#define fseek(file,off,org) _fseeki64(file,off,org)
#define ftell(file) _ftelli64(file)

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

FILE *inf,*outf, *rlogf;
fpos_t blockstart; // use fseek/ftell, because fgetpos/fsetpos is buggy in lcc-win64!
char basefilename[MAXPATH];
long long lines_in = 0;
int numfiles = 0, numblks = 0, numbadparityblks = 0, nummalformedblks = 0, numgoodmultipleblks = 0, numtapemarks = 0;
int numfilebytes, numfileblks;
long long numoutbytes = 0;
bool logging = false;
bool verbose = true;
bool terse = false;
bool quiet = false;
bool filelist = false;
bool multiple_tries = false;
bool deskew = false;
bool doing_deskew = false;
bool tap_format = false;
bool hdr1_label = false;
byte expected_parity = 1;
bool window_set;
double last_sample_time;
int input_permutation[MAXTRKS] = { -1 };

enum mode_t mode = PE;      // default
float bpi= 1600, ips= 50;
int ntrks = 9;

int starting_parmset = 0;
time_t start_time;
int skip_samples = 0;
int dlog_lines = 0;


byte EBCDIC [256] = {/* EBCDIC to ASCII */
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

byte BCD1401 [64] = { // IBM 1401 BCD to ASCII
   /*0x*/ ' ', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '#', '@', ':', '>', 't',  // t=tapemark
   /*1x*/ ' ', '/', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'r', ',', '%', '=', '\'', '"', // r=recordmark
   /*2x*/ '-', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', '!', '$', '*', ')', ';', 'd',  // d=delta symbol
   /*3x*/ '&', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', '?', '.', '?', '(', '<', 'g' };// g=groupmark
// blank is 00 in memory, but x10 on tape

static void vlog(const char *msg, va_list args) {
   vfprintf(stdout, msg, args);  // to the console
   if (logging && rlogf)  // and maybe also the log file
      vfprintf(rlogf, msg, args); }

void rlog(const char* msg, ...) { // regular log
   va_list args;
   va_start(args, msg);
   vlog(msg, args);
   va_end(args); }

void debuglog(const char* msg, ...) { // debugging log
   static bool endmsg_given = false;
   va_list args;
   if (!endmsg_given) {
      if (++dlog_lines < DLOG_LINE_LIMIT) {
         va_start(args, msg);
         vlog(msg, args);
         va_end(args); }
      else {
         rlog("-----> debugging log stopped\n");
         endmsg_given = true; } } }

static void vfatal(const char *msg, va_list args) {
   vlog("\n***FATAL ERROR: ", 0);
   vlog(msg, args);
   rlog("\n");
   //rlog("I/O errno = %d\n", errno);
   exit(99); }

void fatal(const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   vfatal(msg, args);
   va_end(args); }

void assert(bool t, const char *msg, ...) {
   va_list args;
   va_start(args, msg);
   if (!t) vfatal(msg, args);
   va_end(args); }

void SayUsage (void) {
   static char *usage[] = {
      "Use: readtape <options> <basefilename>",
      "   the input file is <basefilename>.csv",
      "   the optional parmset file is <basefilename>.parms",
      "     (or NRZI.parms or PE.parms)",
      "   the output files will be in the created directory <basefilename>\\",
      "   the log file will also be there, as <basefilename>.log",
      "Options:",
      " -ntrks=n  sets the number of tracks; default is 9",
      " -order=   set input data order for bits 0..ntrks-2 and P",
      "           0=MSB; default is 01234567P for 9 trks",
      " -pe       do PE decoding; sets bpi=1600, ips=50",
      " -nrzi     do NRZI decoding; sets bpi=800, ips=50",
      " -gcr      do GCR decoding; sets bpi=6250, ips=25",
      " -bpi=n    override density in bits/inch",
      " -ips=n    override speed in inches/sec",
      " -even     even parity for 7-track NRZI BCD tapes",
      " -skip=n   skip the first n samples",
      " -tap      create one SIMH .tap file from the data",
      " -deskew   do NRZI track deskew based on initial samples",
      " -m        try multiple ways to decode a block",
      " -l        create a log file",
      " -v        verbose mode (all info)",
      " -t        terse mode (only bad block info)",
      " -q        quiet mode (only say \"ok\" or \"bad\")",
      " -f        take file list from <basefilename>.txt",
      NULLP };
   for (int i = 0; usage[i]; ++i) fprintf(stderr, "%s\n", usage[i]); }

bool opt_key(const char* arg, const char* keyword) {
   do { // check for a keyword option
      if (toupper(*arg++) != *keyword++) return false; }
   while (*keyword);
   return *arg == '\0'; }

bool opt_int(const char* arg,  const char* keyword, int *pval, int min, int max) {
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

bool parse_track_order(const char*str) { // examples: P314520, 01234567P
   int bits_done = 0;
   if (strlen(str) != ntrks) return false;
   for (int i = 0; i < ntrks; ++i) {
      byte ch = str[i];
      if (toupper(ch) == 'P') ch = ntrks - 1; // we put parity last
      else {
         if (!isdigit(ch)) return false; // assumes ntrks <= 11
         if ((ch -= '0') > ntrks - 2) return false; }
      input_permutation[i] = ch;
      bits_done |= 1 << ch; }
   return bits_done + 1 == (1 << ntrks); } // must be a permutation of 0..ntrks-1

bool parse_option(char *option) { // (also called from .parm file processor)
   if (option[0] != '/' && option[0] != '-') return false;
   char *arg = option + 1;
   char *str;
   if (opt_int(arg, "NTRKS=", &ntrks, 5, 9));
   else if (opt_str(arg, "ORDER=", &str)
            && parse_track_order(str));
   else if (opt_key(arg, "NRZI")) {
      mode = NRZI; bpi = 800; ips = 50; }
   else if (opt_key(arg, "PE")) {
      mode = PE; bpi = 1600; ips = 50; }
   else if (opt_key(arg, "GCR")) {
      mode = GCR; bpi = 6250; ips = 25; }
   else if (opt_flt(arg, "BPI=", &bpi, 100, 10000));
   else if (opt_flt(arg, "IPS=", &ips, 10, 100));
   else if (opt_int(arg, "SKIP=", &skip_samples, 0, INT_MAX)) {
      if (!quiet) rlog("Will skip the first %d samples\n", skip_samples); }
   else if (opt_key(arg, "TAP")) tap_format = true;
   else if (opt_key(arg, "EVEN")) expected_parity = 0;
   else if (opt_key(arg, "DESKEW")) deskew = true;
   else if (option[2] == '\0') // single-character switches
      switch (toupper(option[1])) {
      case 'H':
      case '?': SayUsage(); exit(1);
      case 'M': multiple_tries = true;  break;
      case 'L': logging = true;  break;
      case 'T': terse = true; verbose = false;  break;
      case 'V': verbose = true; terse = quiet = false;  break;
      case 'Q': quiet = true;  terse = verbose = false;  break;
      case 'F': filelist = true;  break;
      default: goto opterror; }
   else {
opterror:  fatal("bad option: %s\n\n", option);
      // SayUsage();
      exit(4); }
   return true; }

int HandleOptions (int argc, char *argv[]) {
   /* returns the index of the first argument that is not an option;
   i.e. does not start with a dash or a slash */
   int i, firstnonoption = 0;
   //for (i = 0; i < argc; ++i) printf("arg %d: \"%s\"\n", i, argv[i]);
   for (i = 1; i < argc; i++) {
      if (!parse_option(argv[i])) { // end of switches
         firstnonoption = i;
         break; } }
   return firstnonoption; }

bool compare4(uint16_t *d, const char *c) { // 4-character string compare ASCII to EBCDIC
   for (int i=0; i<4; ++i)
      if (EBCDIC[d[i]>>1] != c[i]) return false;
   return true; }

void copy_EBCDIC (byte *to, uint16_t *from, int len) { // copy and translate to ASCII
   while (--len >= 0) to[len] = EBCDIC[from[len]>>1]; }

char *trim(char *p, int len) { // remove trailing blanks
   while (len && p[--len] == ' ') p[len] = '\0';
   return p; }

byte parity (uint16_t val) { // compute the parity of one byte
   byte p = val & 1;
   while (val >>= 1) p ^= val & 1;
   return p; }

int count_parity_errs (uint16_t *data, int len) { // count parity errors in a block
   int parity_errs = 0;
   for (int i=0; i<len; ++i) {
      if (parity(data[i]) != expected_parity) ++parity_errs; }
   return parity_errs; }

int count_faked_bits (uint16_t *data_faked, int len) { // count how many bits we faked for all tracks
   int faked_bits = 0;
   for (int i=0; i<len; ++i) {
      uint16_t v=data_faked[i];
      // This is Brian Kernighan's clever method of counting one bits in an integer
      for (; v; ++faked_bits) v &= v-1; //clr least significant bit set
   }
   return faked_bits; }

int count_faked_tracks(uint16_t *data_faked, int len) { // count how many tracks had faked bits
   uint16_t faked_tracks = 0;
   int c;
   for (int i=0; i<len; ++i) faked_tracks |= data_faked[i];
   for (c=0; faked_tracks; ++c) faked_tracks &= faked_tracks-1;
   return c; }

void show_block_errs (int len) { // report on parity errors and faked bits in all tracks
   for (int i=0; i<len; ++i) {
      byte curparity=parity(data[i]);
      if (curparity != expected_parity || data_faked[i]) { // something wrong with this data
         dlog("  %s parity at byte %4d, time %11.7lf", curparity == expected_parity ? "good" : "bad ", i, data_time[i]);
         if (data_faked[i]) dlog(", faked bits: %03X", data_faked[i]); //Visual Studio doesn't support %b
         dlog("\n"); } } }

void dumpdata (uint16_t *data, int len, bool bcd) { // display a data block in hex and EBCDIC or BCD
   rlog("block length %d\n", len);
   for (int i=0; i<len; ++i) {
      rlog("%02X %d ", data[i]>>1, data[i]&1); // data then parity
      if (i%16 == 15) {
         rlog(" ");
         for (int j=i-15; j<=i; ++j) rlog("%c", bcd ? BCD1401[data[j] >> 1] : EBCDIC[data[j]>>1]);
         rlog("\n"); } }
   if (len%16 != 0) rlog("\n"); }

void output_tap_marker(uint32_t num) {  //output a 4-byte .TAP file marker, little-endian
   for (int i = 0; i < 4; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output_tap_marker");
      num >>= 8; }
   numoutbytes += 4; }

void close_file(void) {
   if (outf) {
      fclose(outf);
      if (!quiet) rlog("file closed at %.7lf with %s bytes written from %d blocks\n", timenow, intcommas(numfilebytes), numfileblks);
      outf = NULLP; } }

void create_file(const char *name) {
   char fullname[MAXPATH];
   if (outf) close_file();
   if (name) { // we generated a name based on tape labels
      assert(strlen(name) < MAXPATH - 5, "create_file name too big 1");
      strcpy(fullname, name);
      strcat(fullname, ".bin"); }
   else { // otherwise create a generic name
      assert(strlen(basefilename) < MAXPATH - 5, "create_file name too big 1");
      if (tap_format)
         sprintf(fullname, "%s\\%s.tap", basefilename, basefilename);
      else sprintf(fullname, "%s\\%03d.bin", basefilename, numfiles+1); }
   if (!quiet) rlog("creating file \"%s\"\n", fullname);
   outf = fopen(fullname, "wb");
   assert(outf, "file create failed for \"%s\"", fullname);
   ++numfiles;
   numfilebytes = numfileblks = 0; }

void got_tapemark(void) {
   ++numtapemarks;
   if (!quiet) rlog("*** tapemark\n");
   blockstart = ftell(inf); // remember the input file position for the start of the next block
   dlog("got tapemark, file pos %s at %.7lf\n", longlongcommas(blockstart), timenow);
   if (tap_format) {
      if (!outf) create_file(NULLP);
      output_tap_marker(0x00000000); }
   else if (!hdr1_label) close_file(); // not tap format: close the file if we didn't see tape labels
   hdr1_label = false; }

void got_datablock(bool malformed) { // decoded a tape block
   int length=block.results[block.parmset].minbits;
   struct results_t *result = &block.results[block.parmset];

   if (!malformed && !tap_format && length==80 && compare4(data,"VOL1")) { // IBM volume header
      struct IBM_vol_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      if (!quiet) {
         rlog("*** tape label %.4, serno \"%.6s\", owner \"%.10s\"\n", hdr.id, trim(hdr.serno,6), trim(hdr.owner,10));
         if (result->errcount) rlog("--> %d errors\n", result->errcount); }
      //dumpdata(data, length, false);
   }
   else if (!malformed &&!tap_format && length==80 && (compare4(data,"HDR1") || compare4(data,"EOF1") || compare4(data,"EOV1"))) {
      struct IBM_hdr1_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      if (!quiet) {
         rlog("*** tape label %.4s, dsid \"%.17s\", serno \"%.6s\", created%.6s\n",
              hdr.id, trim(hdr.dsid,17), trim(hdr.serno,6), trim(hdr.created,6));
         rlog("    volume %.4s, dataset %.4s\n", trim(hdr.volseqno,4), trim(hdr.dsseqno,4));
         if (compare4(data, "EOF1")) rlog("    block count %.6s, system %.13s\n", hdr.blkcnt, trim(hdr.syscode,13));
         if (result->errcount) rlog("--> %d errors\n", result->errcount); }
      //dumpdata(data, length, false);
      if (compare4(data,"HDR1")) { // create the output file from the name in the HDR1 label
         char filename[MAXPATH];
         sprintf(filename, "%s\\%03d-%.17s%c", basefilename, numfiles+1, hdr.dsid, '\0');
         for (int i=strlen(filename); filename[i-1]==' '; --i) filename[i-1]=0;
         if (!tap_format) create_file(filename);
         hdr1_label = true; }
      if (compare4(data,"EOF1") && !tap_format) close_file(); }
   else if (!malformed && !tap_format && length==80 && (compare4(data,"HDR2") || compare4(data,"EOF2") || compare4(data, "EOV2"))) {
      struct IBM_hdr2_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      if (!quiet) {
         rlog("*** tape label %.4s, RECFM=%.1s%.1s, BLKSIZE=%.5s, LRECL=%.5s\n",//
              hdr.id, hdr.recfm, hdr.blkattrib, trim(hdr.blklen,5), trim(hdr.reclen,5));
         rlog("    job: \"%.17s\"\n", trim(hdr.job,17));
         if (result->errcount) rlog("--> %d errors\n", result->errcount); }
      //dumpdata(data, length, false);
   }
   else if (length > 0) { // a normal non-label data block (or in tap_format), but maybe malformed
      if (length <= 2) {
         dlog("*** ingoring runt block of %d bytes at %.7lf\n", length, timenow); }
      else { // decent-sized block
         //dumpdata(data, length, ntrks == 7);
         if (!outf) { // create a generic data file if we didn't see a file header label
            create_file(NULLP); }
         uint32_t errflag = result->errcount ? 0x80000000 : 0;  // SIMH .tap file format error flag
         if (tap_format) output_tap_marker(length | errflag); // leading record length
         for (int i = 0; i < length; ++i) { // discard the parity bit track and write all the data bits
            byte b = data[i] >> 1;
            assert(fwrite(&b, 1, 1, outf) == 1, "write failed"); }
         if (tap_format) {
            byte zero = 0;  // tap format needs an even number of data bytes
            if (length & 1) assert(fwrite(&zero, 1, 1, outf) == 1, "write of odd byte failed");
            numoutbytes += 1;
            output_tap_marker(length | errflag); // trailing record length
         }
         numoutbytes += length;
         numfilebytes += length;
         ++numfileblks;
         ++numblks;
         if (DEBUG && (result->errcount != 0 || result->faked_bits != 0))
            show_block_errs(result->maxbits);
         if (DEBUG && mode == NRZI) {
            if (result->crc_bad) dlog("bad CRC: %03x\n", result->crc);
            if (result->lrc_bad) dlog("bad LRC: %03x\n", result->lrc); }
         if (result->errcount != 0) ++numbadparityblks;
         if (verbose || (terse && (result->errcount > 0 || malformed))) {
            rlog("wrote block %3d, %4d bytes, %d tries, parmset %d, max AGC %.2f, %d parity errs, ",
                 numblks, length, block.tries, block.parmset, result->alltrk_max_agc_gain, result->vparity_errs);
            if (mode == PE)
               rlog("%d faked bits on %d trks", count_faked_bits(data_faked, length), count_faked_tracks(data_faked, length));
            if (mode == NRZI) {
               if (ntrks == 9)
                  rlog("CRC %s ", result->crc_bad ? "bad," : "ok, "); // only 9-track tapes have CRC
               rlog("LRC %s ", result->lrc_bad ? "bad," : "ok, "); }
            rlog(" avg speed %.2f IPS, at time %.7lf\n", 1 / (result->avg_bit_spacing * bpi), timenow); }
         if ((!quiet || terse) && malformed) {
            rlog("   malformed, with lengths %d to %d\n", result->minbits, result->maxbits);
            ++nummalformedblks; } } }
   blockstart = ftell(inf);  // remember the file position for the start of the next block
   //log("got valid block %d, file pos %s at %.7lf\n", numblks, longlongcommas(blockstart), timenow);
};

float scanfast_float(char **p) { // *** fast scanning routines for the CSV numbers in the input file
   float n=0;
   bool negative=false;
   while (**p==' ' || **p==',') ++*p; //skip leading blanks, comma
   if (**p=='-') { // optional minus sign
      ++*p;
      negative = true; }
   while (isdigit(**p)) n = n*10 + (*(*p)++ -'0'); //accumulate left of decimal point
   if (**p=='.') { // skip decimal point
      float divisor=10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ -'0')/divisor;
         divisor *= 10; } }
   return negative ? -n : n; }

double scanfast_double(char **p) {
   double n=0;
   bool negative=false;
   while (**p==' ' || **p==',') ++*p;  //skip leading blanks, comma
   if (**p=='-') { // optional minus sign
      ++*p;
      negative = true; }
   while (isdigit(**p)) n = n*10 + (*(*p)++ -'0'); //accumulate left of decimal point
   if (**p=='.') {
      double divisor=10;
      ++*p;
      while (isdigit(**p)) { //accumulate right of decimal point
         n += (*(*p)++ -'0')/divisor;
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

char *modename(void) {
   return mode == PE ? "PE" : mode == NRZI ? "NRZI" : mode == GCR ? "GCR" : "???"; }

bool readblock(bool retry) { // read the CSV file until we get to the end of a block on the tape
   // return false if we are already at the endfile
   char line[MAXLINE+1];
   struct sample_t sample;
   if (!fgets(line, MAXLINE, inf)) // if we get an immediate endfile
      return false;
   while(1) {
      line[MAXLINE-1]=0;
      if (!retry) ++lines_in;

      /* sscanf is excruciately slow and was taking 90% of the processing time!
      The special-purpose scan routines are about 25 times faster, but do
      no error checking. We replaced the following code:
      items = sscanf(line, " %lf, %f, %f, %f, %f, %f, %f, %f, %f, %f ", &sample.time,
      &sample.voltage[0], &sample.voltage[1], &sample.voltage[2],
      &sample.voltage[3], &sample.voltage[4], &sample.voltage[5],
      &sample.voltage[6], &sample.voltage[7], &sample.voltage[8]);
      assert (items == ntrks+1,"bad CSV line format"); */

      char *linep = line;
      sample.time = scanfast_double(&linep);  // get the time of this sample
      for (int i=0; i<ntrks; ++i) // read voltages for all tracks, and permute as necessary
         sample.voltage[input_permutation[i]] = scanfast_float(&linep);

      if (!window_set && last_sample_time != 0) {
         // we have seen two samples, so set the width of the peak-detect moving window
         sample_deltat = sample.time - last_sample_time;
         pkww_width = min(PKWW_MAX_WIDTH, (int)(PARM.pkww_bitfrac / (bpi*ips*sample_deltat)));
         static said_rates = false;
         if (!quiet && !said_rates) {
            rlog("%s, %d BPI, %d IPS, sampling rate is %s Hz (%.2f usec), initial peak window width is %d samples\n",
                 modename(), (int)bpi, (int)ips, intcommas((int)(1.0 / sample_deltat)), sample_deltat*1e6, pkww_width);
            said_rates = true; }
         window_set = true; }
      last_sample_time = sample.time;

      if (process_sample(&sample) != BS_NONE)  // process one voltage sample point for all tracks
         break;  // until we recognize an end of block
      if (!fgets(line, MAXLINE, inf)) { // read the next line
         process_sample(NULLP); // if we get to the end of the file, force "end of block" processing
         break; } }
   return true; }

bool process_file(int argc, char *argv[]) { // process a complete input file; return TRUE if all blocks were well-formed and had good parity
   char filename[MAXPATH], logfilename[MAXPATH];
   char line[MAXLINE + 1];
   bool ok = true;

   if (mkdir(basefilename) != 0) // create the working directory for output files
      assert(errno == EEXIST || errno == 0, "can't create directory \"%s\", basefilename");

   if (logging) { // Open the log file
      sprintf(logfilename, "%s\\%s.log", basefilename, basefilename);
      assert(rlogf = fopen(logfilename, "w"), "Unable to open log file \"%s\"", logfilename); }

   strncpy(filename, basefilename, MAXPATH - 5);  // open the input file
   filename[MAXPATH - 5] = '\0';
   strcat(filename, ".csv");
   inf = fopen(filename, "r");
   assert(inf, "Unable to open input file \"%s\"", filename);

   if (!quiet) {
      for (int i = 0; i < argc; ++i)  // for documentation in the log, print invocation options
         rlog("%s ", argv[i]);
      rlog("\n");
      rlog("readtape version \"%s\" compiled on %s at %s\n", VERSION, __DATE__, __TIME__);
      rlog("reading file \"%s\" on %s", filename, ctime(&start_time)); // ctime ends with newline!
   }
   read_parms(); // read the .parm file, if anyu

   fgets(line, MAXLINE, inf); // first two (why?) lines in the input file are headers from Saleae
   //log("%s",line);
   fgets(line, MAXLINE, inf);
   //log("%s\n",line);

   if (skip_samples > 0) {
      if (!quiet) rlog("skipping %d samples\n", skip_samples);
      while (skip_samples--)
         assert(fgets(line, MAXLINE, inf), "endfile with %d lines left to skip\n", skip_samples); }
   interblock_expiration = 0;
   starting_parmset = 0;

#if DESKEW     // automatic deskew determination based on the first block
   if (mode == NRZI && deskew) { // currently only for NRZI
      doing_deskew = true;
      init_blockstate(); // do one block
      block.parmset = starting_parmset;
      blockstart = ftell(inf);// remember the file position for the start of a block
      init_trackstate();
      readblock(true);
      nrzi_set_deskew();
      assert(fseek(inf, blockstart, SEEK_SET) == 0, "seek failed at deskew");
      interblock_expiration = 0;
      doing_deskew = false; }
#endif

   while (1) { // keep processing lines of the file
      init_blockstate();  // initialize for a new block
      block.parmset = starting_parmset;
      blockstart = ftell(inf);// remember the file position for the start of a block
      dlog("\n*** block start file pos %s at %.7lf\n", longlongcommas(blockstart), timenow);

      bool keep_trying;
      int last_parmset;
      block.tries = 0;
      do { // keep reinterpreting a block with different parameters until we get a perfect block or we run out of parameter choices
         keep_trying = false;
         ++PARM.tried;  // note that we used this parameter set in another attempt
         last_parmset = block.parmset;
         window_set = false;
         last_sample_time = 0;
         init_trackstate();
         dlog("\n     trying block %d with parmset %d at byte %s at time %.7lf\n", numblks + 1, block.parmset, longlongcommas(blockstart), timenow);
         if (!readblock(block.tries>0)) goto endfile; // ***** read a block ******
         struct results_t *result = &block.results[block.parmset];
         if (result->blktype == BS_NONE) goto endfile; // stuff at the end wasn't a real block
         ++block.tries;
         dlog("     block %d is type %d parmset %d, minlength %d, maxlength %d, %d errors, %d faked bits at %.7lf\n", //
              numblks + 1, result->blktype, block.parmset, result->minbits, result->maxbits, result->errcount, result->faked_bits, timenow);
         if (result->blktype == BS_TAPEMARK) goto done;  // if we got a tapemake, we're done
         if (result->blktype == BS_BLOCK && result->errcount == 0 && result->faked_bits == 0) { // if we got a perfect block, we're done
            if (block.tries>1) ++numgoodmultipleblks;  // bragging rights; perfect blocks due to multiple parameter sets
            goto done; }
         if (multiple_tries && result->minbits != 0) { // if there are no dead tracks (which probably means we saw noise)
            int next_parmset = block.parmset; // then find another parameter set we haven't used yet
            do {
               if (++next_parmset >= MAXPARMSETS) next_parmset = 0; }
            while (next_parmset != block.parmset &&
                   (parmsetsptr[next_parmset].active == 0 || block.results[next_parmset].blktype != BS_NONE));
            //log("found next parmset %d, block_parmset %d, keep_trying %d\n", next_parmset, block.parmset, keep_trying);
            if (next_parmset != block.parmset) { // we have a parmset, so can try again
               keep_trying = true;
               block.parmset = next_parmset;
               dlog("   retrying block %d with parmset %d at byte %s at time %.7lf\n", numblks + 1, block.parmset, longlongcommas(blockstart), timenow);
               assert(fseek(inf, blockstart, SEEK_SET) == 0, "seek failed at retry");
               interblock_expiration = 0; } } }
      while (keep_trying);

      // We didn't succeed in getting a perfect decoding of the block, so pick the best of multiple bad decodings.

      if (block.tries == 1) { // unless we don't have multiple decoding tries
         if (block.results[block.parmset].errcount > 0) ok = false; }
      else {
         dlog("looking for good parity blocks\n");
         int min_bad_bits = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 1: find a decoding with no errors and the minimum number of faked bits
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount == 0 && result->faked_bits<min_bad_bits) {
               min_bad_bits = result->faked_bits;
               block.parmset = i;
               dlog("  best good parity choice is parmset %d\n", block.parmset); } }
         if (min_bad_bits < INT_MAX) goto done;

         ok = false; // we had at least one bad block
         dlog("looking for minimum bad parity blocks\n");
         min_bad_bits = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 2: Find the decoding with the mininum number of errors
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount < min_bad_bits) {
               min_bad_bits = result->errcount;
               block.parmset = i;
               dlog("  best bad parity choice is parmset %d\n", block.parmset); } }
         if (min_bad_bits < INT_MAX) goto done;

         dlog("looking for least malformed blocks\n");
         int min_track_diff = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 3: The block is malformed; find the decoding with the minimum difference in track lengths
            struct results_t *result = &block.results[i];
            int track_diff = result->maxbits - result->minbits;
            if (result->blktype == BS_MALFORMED && track_diff < min_track_diff) {
               min_track_diff = track_diff;
               block.parmset = i;
               dlog("  best malformed block choice is parmset %d\n", block.parmset); } }
         assert(min_track_diff < INT_MAX, "bad malformed block status"); }
done:
      dlog("  chose parmset %d as best after %d tries\n", block.parmset, block.tries);
      ++PARM.chosen;  // count times that this parmset was chosen to be used
      if (block.tries>1 // if we processed the block multiple times
            && last_parmset != block.parmset) { // and the decoding we chose isn't the last one we did
         assert(fseek(inf, blockstart, SEEK_SET) == 0, "seek failed at reprocess"); // then reprocess the chosen one to recompute that best data
         interblock_expiration = 0;
         dlog("     rereading with parmset %d\n", block.parmset);
         init_trackstate();
         assert(readblock(true), "got endfile rereading a block");
         struct results_t *result = &block.results[block.parmset];
         dlog("     reread of block %d with parmset %d is type %d, minlength %d, maxlength %d, %d errors, %d faked bits at %.7lf\n", //
              numblks + 1, block.parmset, result->blktype, result->minbits, result->maxbits, result->errcount, result->faked_bits, timenow); }

      switch (block.results[block.parmset].blktype) {  // process the block according to our best decoding
      case BS_TAPEMARK:
         got_tapemark();
         break;
      case BS_BLOCK:
         got_datablock(false);
         break;
      case BS_MALFORMED:
         got_datablock(true);
         break;
      default:
         fatal("bad block state after decoding", ""); }

#if USE_ALL_PARMSETS
      do { // If we start with a new parmset each time, we'll use them all relatively equally and can see which ones are best
         if (++starting_parmset >= MAXPARMSETS) starting_parmset = 0; }
      while (parmsetsptr[starting_parmset].clk_factor == 0);
#else
      // otherwise we always start with parmset 0, which is the best for most tapes
#endif

   }  // next line of the file
endfile:
   if (tap_format && outf) output_tap_marker(0xffffffffl);
   close_file();
   return ok; }

void breakpoint(void) { // for the debugger
   static int counter;
   ++counter; }

/*-----------------------------------------------------------------
  The basic structure for processing the data is:

 main:
 process_file()
   until EOF do
       init_block()
       for all parameter sets do
          init_tracks()
          readblock: do process_sample() until end_of_block()
      pick the best decoding
      if necessary to regenerate a previous better decoding,
         init_tracks()
         readbock: do process_sample() until end_of_block()
      call got_datablock() or got_tapemark()
         decode standard labels, if any
         write output file data block
---------------------------------------------------------------------*/
void main(int argc, char *argv[]) {
   int argno;
   char *cmdfilename;

#if 0 // compiler checks
   assert(sizeof(struct IBM_vol_t) == 80, "bad vol type");
   assert(sizeof(struct IBM_hdr1_t) == 80, "bad hdr1 type");
   assert(sizeof(struct IBM_hdr2_t) == 80, "bad hdr2 type");
#define showsize(x) printf(#x "=%d bytes\n", (int)sizeof(x));
   showsize(byte);
   showsize(bool);
   showsize(int);
   showsize(long);
   showsize(long long);
   showsize(struct parms_t);
#endif

   // process command-line options

   if (argc == 1) {
      SayUsage();
      exit(4); }
   argno = HandleOptions(argc, argv);
   if (argno == 0) {
      fprintf(stderr, "\n*** No <basefilename> given\n\n");
      SayUsage();
      exit(4); }
   if(argc > argno+1) {
      fprintf(stderr, "\n*** unknown parameter: %s\n\n", argv[argno]);
      SayUsage();
      exit(4); }
   if (input_permutation[0] == -1) // no input value permutation was given
      for (int i = 0; i < ntrks; ++i) input_permutation[i] = i; // create default
   cmdfilename = argv[argno];
   start_time = time(NULL);
   assert(mode != GCR, "GCR is not implemented yet");

   if (filelist) {  // process a list of files
      char filename[MAXPATH], logfilename[MAXPATH];
      char line[MAXLINE + 1];
      strncpy(filename, cmdfilename, MAXPATH - 5);
      filename[MAXPATH - 5] = '\0';
      strcat(filename, ".txt");
      FILE *listf = fopen(filename, "r");
      assert(listf, "Unable to open file list file \"%s\"", filename);
      while (fgets(line, MAXLINE, listf)) {
         line[strcspn(line, "\n")] = 0;
         if (line[0] != 0) {
            strncpy(basefilename, line, MAXPATH - 5);
            basefilename[MAXPATH - 5] = '\0';
            bool result = process_file(argc, argv);
            printf("%s: %s\n", basefilename, result ? "ok" : "bad"); } } }

   else {  // process one file
      strncpy(basefilename, cmdfilename, MAXPATH - 5);
      basefilename[MAXPATH - 5] = '\0';
      bool result = process_file(argc, argv);
      if (quiet) {
         printf("%s: %s\n", basefilename, result ? "ok" : "bad"); }
      else {
         double elapsed_time = difftime(time(NULL), start_time); // integer seconds!?!
         rlog("\n%s samples processed in %.0lf seconds, or %.2lf seconds/block\n",
              longlongcommas(lines_in), elapsed_time, numblks == 0 ? 0 : elapsed_time / numblks);
         rlog("created %d files with %s bytes\n",
              numfiles, longlongcommas(numoutbytes));
         rlog("detected %d tape marks, and %d data blocks of which %d had errors and %d were malformed.\n",//
              numtapemarks, numblks, numbadparityblks, nummalformedblks); } }
   if (!quiet) {
      rlog("%d perfect blocks needed to try more than one parmset\n", numgoodmultipleblks);

      for (int i = 0; i < MAXPARMSETS; ++i) //  // show stats on all the parameter sets we tried
         if (parmsetsptr[i].tried > 0) {
            rlog("parmset %d was tried %4d times and used %4d times, or %5.1f%%: ",
                 i, parmsetsptr[i].tried, parmsetsptr[i].chosen, 100.*parmsetsptr[i].chosen / parmsetsptr[i].tried);

            if (parmsetsptr[i].clk_window) rlog("clk wind %d bits, ", parmsetsptr[i].clk_window);
            else if (parmsetsptr[i].clk_alpha) rlog("clk alpha %.2f, ", parmsetsptr[i].clk_alpha);
            else rlog("clk spacing %.2f usec, ", (mode == PE ? 1/(ips*bpi) : nrzi.clkavg.t_bitspaceavg)*1e6);

            if (parmsetsptr[i].agc_window) rlog("AGC wind %d bits, ", parmsetsptr[i].agc_window);
            else if (parmsetsptr[i].agc_alpha) rlog("AGC alpha %.2f, ", parmsetsptr[i].agc_alpha);
            else rlog("AGC off");

            if (mode == PE)
               rlog("clk factor %.2f, ", parmsetsptr[i].clk_factor);

            rlog("min peak %.2fV, pulse adj %.2f, ww frac %.1f\n",
                 parmsetsptr[i].min_peak, parmsetsptr[i].pulse_adj, parmsetsptr[i].pkww_bitfrac);
            //
         } }
#if PEAK_STATS
   if (mode == NRZI && !quiet) nrzi_output_stats();
#endif
}

//*
