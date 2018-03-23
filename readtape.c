//file: readtape.c
/************************************************************************

Read an IBM formatted 1600 BPI 9-track labelled tape
and create one disk file for each tape file.

The input is a CSV file with 10 columns:a timestamp in seconds,
and then the read head voltage for 9 tracks in the order
MSB...LSB, parity. The first two rows are text column headings.

*************************************************************************
Copyright (C) 2018, Len Shustek

---CHANGE LOG

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

**********************************************************************
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

// under Windows Visual Studio 2017, fseek/ftell use 32-bit integers!
#define fseek(file,off,org) _fseeki64(file,off,org)
#define ftell(file) _ftelli64(file)

#define MAXLINE 400

// The format of IBM standard labeled tape headers
// All fields not reserved are in EBCDIC characters

struct IBM_vol_t {
   char id[4]; 		// "VOL1"
   char serno[6];		// volume serial number
   char rsvd1[31];
   char owner[10];		// owner in EBCDIC
   char rxvd2[29]; };
struct IBM_hdr1_t {
   char id[4];			// "HDR1" or "EOF1" or "EOV1"
   char dsid[17];		// dataset identifier
   char serno[6];		// dataset serial number
   char volseqno[4];	// volume sequence numer
   char dsseqno[4];	// dataset sequence number
   char genno[4];		// generation number
   char genver[2];		// version number of generation
   char created[6];	// creation date as yyddd
   char expires[6];	// expiration date as yyddd
   char security[1];	// security: 0=none, 1=rwe, 2=we
   char blkcnt[6];		// block count (in EOF)
   char syscode[13];	// system code
   char rsvd1[7]; };
struct IBM_hdr2_t {
   char id[4];			// "HDR2" or "EOF2" or "EOV2"
   char recfm[1];		// F, V, or U
   char blklen[5];		// block length
   char reclen[5];		// record length
   char density[1];	// 3 for 1600 BPI
   char dspos[1];		// dataset position
   char job[17];		// job and job step id
   char recording[2];	// blank for 9-track tape: "odd, no translate"
   char controlchar[1];// A for ASCII, M for machine, blank for none
   char rsvd1[1];
   char blkattrib[1]; 	// B=blocked, S=spanned, R=both, b=neither
   char rsvd2[41]; };

FILE *inf,*outf, *rlogf;
fpos_t blockstart; // fgetpos/fsetpos is buggy in lcc-win64!
char basefilename[MAXPATH];
int lines_in=0, lines_out=0;
int numfiles=0, numblks=0, numbadparityblks=0, nummalformedblks=0, numgoodmultipleblks=0, numtapemarks=0;
int numfilebytes;
bool logging=false;
bool terse=false;
bool verbose=DEBUG;
bool quiet = false;
bool filelist = false;
bool multiple_tries = false;
bool tap_format = false;
bool hdr1_label = false;
enum mode_t mode = PE;
int starting_parmset=0;
time_t start_time;
int skip_samples = 0;
int dlog_lines = 0;


byte EBCDIC [256] = {	/* EBCDIC to ASCII */
   /* 0 */	 ' ',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 8 */	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 10*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 18*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 20*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 28*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 30*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 38*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 40*/	 ' ',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 48*/	 '?',  '?',  '[',  '.',  '<',  '(',  '+',  '|',
   /* 50*/	 '&',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 58*/	 '?',  '?',  '!',  '$',  '*',  ')',  ';',  '^',
   /* 60*/	 '-',  '/',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 68*/	 '?',  '?',  '|',  ',',  '%',  '_',  '>',  '?',
   /* 70*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 78*/	 '?',  '`',  ':',  '#',  '|',  '\'',  '=', '"',
   /* 80*/	 '?',  'a',  'b',  'c',  'd',  'e',  'f',  'g',
   /* 88*/	 'h',  'i',  '?',  '?',  '?',  '?',  '?',  '?',
   /* 90*/	 '?',  'j',  'k',  'l',  'm',  'n',  'o',  'p',
   /* 98*/	 'q',  'r',  '?',  '?',  '?',  '?',  '?',  '?',
   /* a0*/	 '?',  '~',  's',  't',  'u',  'v',  'w',  'x',
   /* a8*/	 'y',  'z',  '?',  '?',  '?',  '?',  '?',  '?',
   /* b0*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* b8*/	 '?',  '?',  '?',  '?',  '?',  '?',  '?',  '?',
   /* c0*/	 '{',  'A',  'B',  'C',  'D',  'E',  'F',  'G',
   /* c8*/	 'H',  'I',  '?',  '?',  '?',  '?',  '?',  '?',
   /* d0*/	 '}',  'J',  'K',  'L',  'M',  'N',  'O',  'P',
   /* d8*/	 'Q',  'R',  '?',  '?',  '?',  '?',  '?',  '?',
   /* e0*/	 '\\', '?',  'S',  'T',  'U',  'V',  'W',  'X',
   /* e8*/	 'Y',  'Z',  '?',  '?',  '?',  '?',  '?',  '?',
   /* f0*/	 '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',
   /* f8*/	 '8',  '9',  '?',  '?',  '?',  '?',  '?',  ' ' };

static void vlog(const char *msg, va_list args) {
   vfprintf(stdout, msg, args);
   if (logging && rlogf)
      vfprintf(rlogf, msg, args); }
void rlog(const char* msg,...) { // write to the console and maybe a log file
   va_list args;
   va_start(args, msg);
   vlog(msg, args);
   va_end(args); }
static void vfatal(const char *msg, va_list args) {
   vlog("***FATAL ERROR: ", 0);
   vlog(msg, args);
   rlog("\nI/O errno = %d\n", errno);
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

void SayUsage (char *programName) {
   static char *usage[] = {
      "Use: readtape <options> <basefilename>",
      "   input file is <basefilename>.csv",
      "   output files will be in the created directory <basefilename>\\",
      "   log file will also be there, as <basefilename>.log",
      "Options:",
      " -pe      do PE decoding (the default)",
      " -nrzi    do NRZI decoding",
      " -gcr     do GCR decoding",
      " -br=n    for NRZI, the expected bit rate in hertz",
      " -skip=n  skip the first n samples",
      " -tap     create one SIMH .tap file from the data",
      " -m       try multiple ways to decode a block",
      " -l       create a log file",
      " -v       verbose mode (extra info)",
      " -t       terse mode (no block info)",
      " -q       quiet mode (only \"ok\" or \"bad\")",
      " -f       take file list from <basefilename>.txt",
      NULL };
   int i = 0;
   while (usage[i] != NULL) fprintf (stderr, "%s\n", usage[i++]); }

int HandleOptions (int argc, char *argv[]) {
   /* returns the index of the first argument that is not an option; i.e.
   does not start with a dash or a slash */
   int i, firstnonoption = 0;
   for (i = 1; i < argc; i++) {
      if (argv[i][0] == '/' || argv[i][0] == '-') {
         // This has gotten out of hand. Need to redo as a table...
         if      (strcmp(argv[i] + 1, "nrzi") == 0) mode = NRZI;
         else if (strcmp(argv[i] + 1, "gcr") == 0) mode = GCR;
         else if (strcmp(argv[i] + 1, "pe") == 0) mode = PE;
         else if (strcmp(argv[i] + 1, "tap") == 0) tap_format = true;
         else if (strncmp(argv[i] + 1, "br=", 3) == 0) {
            int nch, num;
            if (sscanf(argv[i] + 4, "%d%n",&num, &nch) != 1
                  || num < 1000 || num > 100000)
               goto opterror;
            if (argv[i][4 + nch] != '\0') goto opterror;
            nrzi_starting_bitrate = num;
            rlog("NRZI initial bit rate is %f Hz\n", nrzi_starting_bitrate); }
         else if (strncmp(argv[i] + 1, "skip=", 5) == 0) {
            int nch, num;
            if (sscanf(argv[i] + 6, "%d%n", &skip_samples, &nch) != 1)
               goto opterror;
            if (argv[i][6 + nch] != '\0') goto opterror; }
         else
            switch (toupper (argv[i][1])) {
            case 'H':
            case '?': SayUsage (argv[0]); exit (1);
            case 'M': multiple_tries = true;  break;
            case 'L': logging = true;  break;
            case 'T': terse = true; verbose = false;  break;
            case 'V': verbose = true;  break;
            case 'Q': quiet = terse = true;  verbose = false; break;
            case 'F': filelist = true;  break;
            /* add more  option switches here */
            default:
opterror:
               fprintf (stderr, "\n*** unknown option: %s\n\n", argv[i]);
               SayUsage (argv[0]);
               exit (4); } }
      else { // end of switches
         firstnonoption = i;
         break; } }
   return firstnonoption; }

bool compare4(uint16_t *d, const char *c) { // string compare ASCII to EBCDIC
   for (int i=0; i<4; ++i)
      if (EBCDIC[d[i]>>1] != c[i]) return false;
   return true; }

void copy_EBCDIC (byte *to, uint16_t *from, int len) { // copy and translate to ASCII
   while (--len >= 0) to[len] = EBCDIC[from[len]>>1]; }

byte parity (uint16_t val) { // compute the parity of one byte
   byte p = val & 1;
   while (val >>= 1) p ^= val & 1;
   return p; }

int count_parity_errs (uint16_t *data, int len) { // count parity errors in a block
   int parity_errs = 0;
   for (int i=0; i<len; ++i) {
      if (parity(data[i]) != 1) ++parity_errs; }
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
      if (curparity != 1 || data_faked[i]) { // something wrong with this data
         dlog("  %s parity at byte %4d, time %11.7lf", curparity ? "good" : "bad ", i, data_time[i]);
         if (data_faked[i]) dlog(", faked bits: %03X", data_faked[i]); //Visual Studio doesn't support %b
         dlog("\n"); } } }

void dumpdata (uint16_t *data, int len) { // display a data block in hex and EBCDIC
   rlog("block length %d\n", len);
   for (int i=0; i<len; ++i) {
      rlog("%02X ", data[i]>>1);
      if (i%16 == 15) {
         rlog(" ");
         for (int j=i-15; j<=i; ++j) rlog("%c", EBCDIC[data[j]>>1]);
         rlog("\n"); } }
   if (len%16 != 0) rlog("\n"); }

void output_tap_marker(uint32_t num) {  //output a 4-byte .TAP file marker, little-endian
   for (int i = 0; i < 4; ++i) {
      byte lsb = num & 0xff;
      assert(fwrite(&lsb, 1, 1, outf) == 1, "fwrite failed in output_tap_marker");
      num >>= 8; } }

void close_file(void) {
   if (outf) {
      fclose(outf);
      if (!quiet) rlog("file closed with %s bytes written\n", intcommas(numfilebytes));
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
      else sprintf(fullname, "%s\\%03d.bin", basefilename, numfiles); }
   if (!quiet) rlog("creating file \"%s\"\n", fullname);
   outf = fopen(fullname, "wb");
   assert(outf, "file create failed for \"%s\"", fullname);
   ++numfiles;
   numfilebytes = 0; }

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

   if (!malformed && length==80 && compare4(data,"VOL1")) { // IBM volume header
      struct IBM_vol_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      if (!quiet) {
         rlog("*** tape label %.4s: %.6s, owner %.10s\n", hdr.id, hdr.serno, hdr.owner);
         if (result->errcount) rlog("--> %d errors\n", result->errcount); }
      //dumpdata(data, length);
   }
   else if (!malformed && length==80 && (compare4(data,"HDR1") || compare4(data,"EOF1") || compare4(data,"EOV1"))) {
      struct IBM_hdr1_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      if (!quiet) {
         rlog("*** tape label %.4s: %.17s, serno %.6s, created%.6s\n", hdr.id, hdr.dsid, hdr.serno, hdr.created);
         rlog("    volume %.4s, dataset %.4s\n", hdr.volseqno, hdr.dsseqno);
         if (compare4(data, "EOF1")) rlog("    block count: %.6s, system %.13s\n", hdr.blkcnt, hdr.syscode);
         if (result->errcount) rlog("--> %d errors\n", result->errcount); }
      //dumpdata(data, length);
      if (compare4(data,"HDR1")) { // create the output file from the name in the HDR1 label
         char filename[MAXPATH];
         sprintf(filename, "%s\\%03d-%.17s%c", basefilename, numfiles, hdr.dsid, '\0');
         for (int i=strlen(filename); filename[i-1]==' '; --i) filename[i-1]=0;
         if (!tap_format) create_file(filename);
         hdr1_label = true; }
      if (compare4(data,"EOF1") && !tap_format) close_file(); }
   else if (!malformed && length==80 && (compare4(data,"HDR2") || compare4(data,"EOF2") || compare4(data, "EOV2"))) {
      struct IBM_hdr2_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      if (!quiet) {
         rlog("*** tape label %.4s: RECFM=%.1s%.1s, BLKSIZE=%.5s, LRECL=%.5s\n",//
              hdr.id, hdr.recfm, hdr.blkattrib, hdr.blklen, hdr.reclen);
         rlog("    job: %.17s\n", hdr.job);
         if (result->errcount) rlog("--> %d errors\n", result->errcount); }
      //dumpdata(data, length);
   }
   else if (length>0) { // a normal non-label data block, but maybe malformed
      //dumpdata(data, length);
      if(!outf) { // create a generic data file if we didn't see a file header label
         create_file(NULLP); }
      uint32_t errflag = result->errcount ? 0x80000000 : 0;  // SIMH .tap file format error flag
      if (tap_format) output_tap_marker(length | errflag); // leading record length
      for (int i=0; i<length; ++i) { // discard the parity bit track and write all the data bits
         byte b = data[i]>>1;
         assert(fwrite(&b, 1, 1, outf) == 1, "write failed"); }
      if (tap_format) {
         byte zero = 0;  // tap format needs an even number of data bytes
         if (length & 1) assert(fwrite(&zero, 1, 1, outf) == 1, "write of odd byte failed");
         output_tap_marker(length | errflag); // trailing record length
      }
      numfilebytes += length;
      ++numblks;
      if(DEBUG && (result->errcount != 0 || result->faked_bits != 0))
         show_block_errs(result->maxbits);
      if (DEBUG && mode == NRZI) {
         if (result->crc_bad) dlog("bad CRC: %03x\n", result->crc);
         if (result->lrc_bad) dlog("bad LRC: %03x\n", result->lrc); }
      if (result->errcount != 0) ++numbadparityblks;
      if (!terse) {
         if (mode == PE)
            rlog("wrote block %3d, %4d bytes, %d tries, parms %d, max AGC %.2f, %d parity errs, %d faked bits on %d trks, at time %.7lf\n", //
                 numblks, length, block.tries, block.parmset, result->alltrk_max_agc_gain, result->vparity_errs, //
                 count_faked_bits(data_faked, length), count_faked_tracks(data_faked, length), timenow);
         if (mode == NRZI)
            rlog("wrote block %3d, %4d bytes, %d tries, parms %d, max AGC %.2f, %d parity errs, CRC %s, LRC %s, avg speed %.2f IPS, at time %.7lf\n", //
                 numblks, length, block.tries, block.parmset, result->alltrk_max_agc_gain, result->vparity_errs, //
                 result->crc_bad ? "bad":"ok ", result->lrc_bad ? "bad":"ok ", 1/(result->avg_bit_spacing * bpi), timenow);

      }
      if (!quiet && malformed) {
         rlog("   malformed, with lengths %d to %d\n", result->minbits, result->maxbits);
         ++nummalformedblks; } }
   blockstart = ftell(inf);  // remember the file position for the start of the next block
   //log("got valid block %d, file pos %s at %.7lf\n", numblks, longlongcommas(blockstart), timenow);
};

float scan_float(char **p) { // *** fast scanning routines for the CSV numbers in the input file
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
double scan_double(char **p) {
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
// *** THEY USE A STATIC BUFFER, SO YOU CAN ONLY DO ONE CALL PER LINE!
char *intcommas(int n) { // 32 bits
   assert(n >= 0, "bad call to intcommas");
   static char buf[14]; //max: 2,147,483,647
   char *p = buf + 13;
   int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ','; ctr = 3; }
         *p-- = n % 10 + '0';
         n = n / 10; }
   return p + 1; }
char *longlongcommas(long long n) { // 64 bits
   assert(n >= 0, "bad call to longlongcommas");
   static char buf[26]; //max: 9,223,372,036,854,775,807
   char *p = buf + 25;
   int ctr = 4;
   *p-- = '\0';
   if (n == 0)  *p-- = '0';
   else while (n > 0) {
         if (--ctr == 0) {
            *p-- = ',';  ctr = 3; }
         *p-- = n % 10 + '0';
         n = n / 10; }
   return p + 1; }

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
      assert (items == NTRKS+1,"bad CSV line format"); */
      char *linep = line;
      sample.time = scan_double(&linep);
      for (int i=0; i<NTRKS; ++i) sample.voltage[i] =
#if INVERTED
            -
#endif
            scan_float(&linep);
      if (process_sample(&sample) != BS_NONE)	 // process one voltage sample point for all tracks
         break;								 // until we recognize an end of block
      if (!fgets(line, MAXLINE, inf)) { // read the next line
         process_sample(NULLP); // if we get to the end of the file, force "end of block" processing
         break; } }
   return true; }

bool process_file(void) { // process a complete file; return TRUE if all blocks were well-formed and had good parity
   char filename[MAXPATH], logfilename[MAXPATH];
   char line[MAXLINE + 1];
   bool ok = true;

   // open the input file and create the working directory for output files

   strncpy(filename, basefilename, MAXPATH - 5);
   filename[MAXPATH - 5] = '\0';
   strcat(filename, ".csv");
   if (!quiet) {
      rlog("\nreading file  \"%s\" on %s", filename, ctime(&start_time));//
   }
   inf = fopen(filename, "r");
   assert(inf, "Unable to open input file \"%s\"", filename);
   if (mkdir(basefilename) != 0) assert(errno == EEXIST || errno == 0, "can't create directory \"%s\", basefilename");

   if (logging) { // Open the log file
      sprintf(logfilename, "%s\\%s.log", basefilename, basefilename);
      assert(rlogf = fopen(logfilename, "w"), "Unable to open log file \"%s\"", logfilename); }

   fgets(line, MAXLINE, inf); // first two lines in the input file are headers from Saleae
   //log("%s",line);
   fgets(line, MAXLINE, inf);
   //log("%s\n",line);

   if (skip_samples > 0) {
      rlog("skipping %d samples\n", skip_samples);
      while (skip_samples--)
         assert(fgets(line, MAXLINE, inf), "endfile with %d lines left to skip\n", skip_samples); }

   while (1) { // for all lines of the file
      init_blockstate();  // initialize for a new block
      block.parmset = starting_parmset;
      blockstart = ftell(inf);// remember the file position for the start of a block
      dlog("\n*** block start file pos %s at %.7lf\n", longlongcommas(blockstart), timenow);

      bool keep_trying;
      int last_parmset;
      block.tries = 0;
      do { // keep reinterpreting a block with different parameters until we get a perfect block or we run out of parameter choices
         keep_trying = false;
         ++parmsetsptr[block.parmset].tried;  // note that we used this parameter set in another attempt
         last_parmset = block.parmset;
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
               assert(fseek(inf, blockstart, SEEK_SET) == 0, "seek failed 1"); } } }
      while (keep_trying);

      // We didn't succeed in getting a perfect decoding of the block, so pick the best of the bad decodings.

      if (block.tries > 1) {
         dlog("looking for good parity blocks\n");
         int min_bad_bits = INT_MAX;
         for (int i = 0; i<MAXPARMSETS; ++i) { // Try 1: find a decoding with no errors and the minimum number of faked bits
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->errcount == 0 && result->faked_bits<min_bad_bits) {
               min_bad_bits = result->faked_bits;
               block.parmset = i;
               dlog("  best good parity choice is parmset %d\n", block.parmset); } }
         if (min_bad_bits < INT_MAX) goto done;
         ok = false; // we had at least one bad bock

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
      ++parmsetsptr[block.parmset].chosen;  // count times that this parmset was chosen to be used
      if (block.tries>1 // if we processed the block multiple times
            && last_parmset != block.parmset) { // and the decoding we chose isn't the last one we did
         assert(fseek(inf, blockstart, SEEK_SET) == 0, "seek failed 2"); // then reprocess the chosen one to retrieve the data
         dlog("     rereading parmset %d\n", block.parmset);
         init_trackstate();
         assert(readblock(true), "got endfile rereading a block");
         struct results_t *result = &block.results[block.parmset];
         dlog("     reread of block %d with parmset %d is type %d, minlength %d, maxlength %d, %d errors, %d faked bits at %.7lf\n", //
              numblks + 1, block.parmset, result->blktype, result->minbits, result->maxbits, result->errcount, result->faked_bits, timenow); }
      switch (block.results[block.parmset].blktype) {  // process the block according to our best decoding, block.parmset
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
      do { // If we start with a new parm set each time, we'll use them all relatively equally and can see which ones are best
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
    init_block()
    for all parameter sets do
       init_tracks()
       readblock: do process_sample until end_of_block()
   pick best decoding
   reread block if necessary
   got_datablock(), or got_tapemark()
      decode standard labels, if any
      write data block
---------------------------------------------------------------------*/
void main(int argc, char *argv[]) {
   int argno;
   char *cmdfilename;

#if 0 // compiler check
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
      SayUsage(argv[0]);
      exit(4); }
   argno = HandleOptions(argc, argv);
   if (argno == 0) {
      fprintf(stderr, "\n*** No <basefilename> given\n\n");
      SayUsage(argv[0]);
      exit(4); }
   cmdfilename = argv[argno];
   start_time = time(NULL);
   assert(mode != GCR, "GCR is not implemented yet");
   if (mode == PE) {
      bpi = 1600;
      parmsetsptr = parmsets_PE; }
   if (mode == NRZI) {
      bpi = 800;
      parmsetsptr = parmsets_NRZI; }
   if (mode == GCR) {
      bpi = 9042; // the real bit rate for "6250 BPI" tapes
      parmsetsptr = parmsets_GCR; }

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
            bool result = process_file();
            printf("%s: %s\n", basefilename, result ? "ok" : "bad"); } } }

   else {  // process one file
      strncpy(basefilename, cmdfilename, MAXPATH - 5);
      basefilename[MAXPATH - 5] = '\0';
      bool result = process_file();
      if (quiet) {
         printf("%s: %s\n", basefilename, result ? "ok" : "bad"); }
      else {
         double elapsed_time = difftime(time(NULL), start_time); // integer seconds!?!
         rlog("\n%s samples processed in %.0lf seconds\n"
              "created %d files and averaged %.2lf seconds/block\n" //
              "detected %d tape marks, and %d data blocks of which %d had errors and %d were malformed.\n",//
              intcommas(lines_in), elapsed_time,
              numfiles, numblks == 0 ? 0 : elapsed_time / numblks, //
              numtapemarks, numblks, numbadparityblks, nummalformedblks); } }
   if (verbose) {
      rlog("%d perfect blocks needed to try more than one parm set\n", numgoodmultipleblks);
      for (int i = 0; i < MAXPARMSETS; ++i) //
         if (parmsetsptr[i].tried > 0) {
            rlog("parm set %d was tried %4d times and used %4d times, or %5.1f%%: ",
                 i, parmsetsptr[i].tried, parmsetsptr[i].chosen, 100.*parmsetsptr[i].chosen / parmsetsptr[i].tried);
            if (parmsetsptr[i].clk_window) rlog("clk avg wind %d bits, ", parmsetsptr[i].clk_window);
            else if (parmsetsptr[i].clk_alpha) rlog("clk avg alpha %.2f, ", parmsetsptr[i].clk_alpha);
            else rlog("clk spacing %.2f usec, ", (mode == PE ? BIT_SPACING : nrzi.clkavg.t_bitspaceavg)*1e6);
            rlog("move thresh %.2fV, ", parmsetsptr[i].move_threshold);
            rlog("zero band %.2fV, ", parmsetsptr[i].zero_band);
            if (mode == PE)
               rlog("clk factor %.1f, pulse adj %.2f",//
                    parmsetsptr[i].clk_factor, parmsetsptr[i].pulse_adj_amt);
            rlog("\n");
            //
         } } }

//*
