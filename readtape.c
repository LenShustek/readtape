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

*** 09 Feb 2018, L. Shustek	 Replace sscanf for a 20x speedup!

Also made major changes to the decoding algorithm:

In the voltage domain, do idle detection based on peak-to-peak
voltage, not proximity to a baseline resting level.

In the time domain, do clock simulation when the signal drops out.
In other words,,we fake bits when we see no flux transitions.

*** 12 Feb 2018, L. Shustek	 Major restructure:

Allow blocks to be processed multiple times with different sets of
deocding parameters. We then choose the best of the bad decodings.

*** 19 Feb 2018, L. Shustek. Change to the peak detection algorithm:

Interpolate the time of the peak when multiple samples are close to
the peak. This significantly improves decoding for low sampling rates.
Sampling at 781 KS/s for 1600 BPI PE tapes running at 50 IPS generates
about 10 samples per bit, and seems to work fine now.

*** 20 Feb 2018, L. Shustek,

Add the "move threshold" to the variable parameters, because setting it
low for some blocks lets us correctly decode some really grungy data.

*** 26 Feb 2018, L. Shustek

Improve the decsion rules for how many fake bit to add during a
dropout. Should we make the strategy choice a variable parameter?

Add Automatic Gain Control (AGC) on a track-by-track basis.
That helps a lot in decoding dropouts without picking up all
sorts of noise between data blocks that results from reducing
the move threshold parameter that applies to all tracks.

Use ASTYLE's Python-like formatting to maximum algorithmic protein
visible on a screen and minimize the clutter of syntatic sugar.

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

#include <decoder.h>
#include <locale.h>

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

FILE *inf,*outf, *logf;
fpos_t blockstart; // fgetpos/fsetpos is buggy in lcc-win64!
char *basefilename;
int lines_in=0, lines_out=0;
int numfiles=0, numblks=0, numbadparityblks=0, nummalformedblks=0, numgoodmultipleblks=0, numtapemarks=0;
int numfilebytes;
bool logging=false;
bool terse=false;
bool verbose=DEBUG;
int starting_parmset=0;

extern double timenow;
extern struct parms_t parmsets[]; // sets of decoding parameters
extern struct blkstate_t block;
extern uint16_t data[];
extern uint16_t data_faked[];
extern double data_time[];
extern float alltrk_max_agc_gain;

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



#if 0 // passing a va_list crashes sometimes, and I don't know why
static void vlog(const char *msg, va_list args) {
   vfprintf(stdout, msg, args);
   if (logging && logf)
      vfprintf(logf, msg, args); }
static void vfatal(const char *msg, va_list args) {
   vlog(msg, args);
   log("\nI/O errno = %d\n", errno);
   exit(99); }
void log(const char* msg,...) { // write to the console and maybe a log file
   va_list args;
   va_start(args, msg);
   vlog(msg, args);
   va_end(args); }
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
#endif

void log(const char* format,...) { // write to the console and maybe a log file
   va_list argptr;
   va_start(argptr, format);
   vfprintf(stdout, format, argptr);
   va_end(argptr);
   if (logging && logf) {
      va_start(argptr, format);
      vfprintf(logf, format, argptr);
      va_end(argptr); } }
void fatal(char *msg1, char *msg2) {
   log("%s %s\n", msg1, msg2);
   log("errno = %d\n", errno);
   exit(99); }
void assert(bool t, char *msg) {
   if (!t) fatal("** FATAL ERROR **", msg); }

void SayUsage (char *programName) {
   static char *usage[] = {
      "Use: readtape <options> <basefilename>",
      "   input file will be <basefilename>.csv",
      "   output files will be in the created directory <basefilename>\\",
      "   log file will also be there, as <basefilename>.log",
      "Options:",
      "  -l   create log file",
      "  -t   terse mode",
      "  -v   verbose mode",
      NULL };
   int i = 0;
   while (usage[i] != NULL) fprintf (stderr, "%s\n", usage[i++]); }
int HandleOptions (int argc, char *argv[]) {
   /* returns the index of the first argument that is not an option; i.e.
   does not start with a dash or a slash */
   int i, nch, firstnonoption = 0;
   for (i = 1; i < argc; i++) {
      if (argv[i][0] == '/' || argv[i][0] == '-') {
         switch (toupper (argv[i][1])) {
         case 'H':
         case '?':
            SayUsage (argv[0]);
            exit (1);
         case 'L':
            logging = true;
            break;
         case 'T':
            terse = true;
            verbose = false;
            break;
         case 'V':
            verbose = true;
            terse = false;
            break;
            /* add more  option switches here */
opterror:
         default:
            fprintf (stderr, "\n*** unknown option: %s\n\n", argv[i]);
            SayUsage (argv[0]);
            exit (4); } }
      else {
         firstnonoption = i;
         break; } }
   return firstnonoption; }


bool compare4(uint16_t *d, char *c) { // string compare ASCII to EBCDIC
   for (int i=0; i<4; ++i)
      if (EBCDIC[d[i]>>1] != c[i]) return false;
   return true; }

void copy_EBCDIC (byte *to, uint16_t *from, int len) { // copy and translate to ASCII
   while (--len >= 0) to[len] = EBCDIC[from[len]>>1]; }

byte parity (uint16_t val) { // compute te parity of one byte
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
         if (data_faked[i]) dlog(", faked bits: %09b", data_faked[i]);
         dlog("\n"); } } }

void dumpdata (uint16_t *data, int len) { // display a data block in hex and EBCDIC
   log("block length %d\n", len);
   for (int i=0; i<len; ++i) {
      log("%02X ", data[i]>>1);
      if (i%16 == 15) {
         log(" ");
         for (int j=i-15; j<=i; ++j) log("%c", EBCDIC[data[j]>>1]);
         log("\n"); } }
   if (len%16 != 0) log("\n"); }

void got_tapemark(void) {
   ++numtapemarks;
   log("\n*** tapemark\n");
   blockstart = ftell(inf); // remember the file position for the start of the next block
   dlog("got tapemark, file pos %'lld at %.7lf\n", blockstart, timenow); }

void close_file(void) {
   if(outf) {
      fclose(outf);
      if(!terse) log("file closed with %'d bytes written\n", numfilebytes);
      outf = NULLP; } }

void createfile(char *name) {
   strcat(name, ".bin");
   if (!terse) log("\ncreating file \"%s\"\n", name);
   outf = fopen(name, "wb");
   assert (outf, "file create failed");
   numfilebytes = 0; }

void got_datablock(bool malformed) { // decoded a tape block
   int length=block.results[block.parmset].minbits;
   struct results_t *result = &block.results[block.parmset];

   if (!malformed && length==80 && compare4(data,"VOL1")) { // IBM volume header
      struct IBM_vol_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      log("\n*** tape label %.4s: %.6s, owner %.10s\n", hdr.id, hdr.serno, hdr.owner);
      if (result->parity_errs) log("--> %d parity errors\n", result->parity_errs);
      //dumpdata(data, length);
   }
   else if (!malformed && length==80 && (compare4(data,"HDR1") || compare4(data,"EOF1") || compare4(data,"EOV1"))) {
      struct IBM_hdr1_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      log("\n*** tape label %.4s: %.17s, serno %.6s, created%.6s\n", hdr.id, hdr.dsid, hdr.serno, hdr.created);
      log("    volume %.4s, dataset %.4s\n", hdr.volseqno, hdr.dsseqno);
      if (compare4(data,"EOF1")) log("    block count: %.6s, system %.13s\n", hdr.blkcnt, hdr.syscode);
      if (result->parity_errs) log("--> %d parity errors\n", result->parity_errs);
      //dumpdata(data, length);
      if (compare4(data,"HDR1")) { // create the output file from the name in the HDR1 label
         char filename[MAXPATH];
         int i;
         sprintf(filename, "%s\\%03d-%.17s%c", basefilename, numfiles++, hdr.dsid, '\0');
         for (int i=strlen(filename); filename[i-1]==' '; --i) filename[i-1]=0;
         createfile(filename); }
      if (compare4(data,"EOF1")) close_file(); }
   else if (!malformed && length==80 && (compare4(data,"HDR2") || compare4(data,"EOF2") || compare4(data, "EOV2"))) {
      struct IBM_hdr2_t hdr;
      copy_EBCDIC((byte *)&hdr, data, 80);
      log("\n*** tape label %.4s: RECFM=%.1s%.1s, BLKSIZE=%.5s, LRECL=%.5s\n",//
          hdr.id, hdr.recfm, hdr.blkattrib, hdr.blklen, hdr.reclen);
      log("    job: %.17s\n", hdr.job);
      if (result->parity_errs) log("--> %d parity errors\n", result->parity_errs);
      //dumpdata(data, length);
   }
   else if (length>0) { // a normal non-label data block, but maybe malformed
      //dumpdata(data, length);
      if(!outf) { // create a generic data file if we didn't see a file header label
         char filename[MAXPATH];
         sprintf(filename, "%s\\%03d", basefilename, numfiles++);
         createfile(filename); }
      for (int i=0; i<length; ++i) { // discard the parity bit track and write all the data bits
         byte b = data[i]>>1;
         assert(fwrite(&b, 1, 1, outf) == 1, "write failed"); }
      numfilebytes += length;
      ++numblks;
      if(result->parity_errs != 0 || result->faked_bits != 0) {
         if (DEBUG) show_block_errs(result->maxbits);
         ++numbadparityblks; }
      log("wrote  block %d, %d bytes, %d tries, parmset %d, max AGC %.2f, %d parity errs, %d faked bits on %d trks, at time %.7lf\n", //
          numblks, length, block.tries, block.parmset, alltrk_max_agc_gain, result->parity_errs, //
          count_faked_bits(data_faked,length), count_faked_tracks(data_faked,length), timenow);
      if (malformed) {
         log("   malformed, with lengths %d to %d\n", result->minbits, result->maxbits);
         ++nummalformedblks; } }
   blockstart = ftell(inf);  // remember the file position for the start of the next block
   //log("got valid block %d, file pos %'lld at %.7lf\n", numblks, blockstart, timenow);
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

bool readblock(void) { // read the CSV file until we get to the end of a block on the tape
   // return false if we are already at the endfile
   char line[MAXLINE+1];
   struct sample_t sample;
   if (!fgets(line, MAXLINE, inf)) // if we get an immediate endfile
      return false;
   while(1) {
      line[MAXLINE-1]=0;
      ++lines_in;
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

void breakpoint(void) { // for the debugger
   static int counter;
   ++counter; }

void main(int argc, char *argv[]) {
   int argno;
   char filename[MAXPATH], logfilename[MAXPATH];
   char line[MAXLINE+1];

#if 0 // compiler check
   assert(sizeof(struct IBM_vol_t)==80, "bad vol type");
   assert(sizeof(struct IBM_hdr1_t)==80, "bad hdr1 type");
   assert(sizeof(struct IBM_hdr2_t)==80, "bad hdr2 type");
#define showsize(x) printf(#x "=%d bytes\n", sizeof(x));
   showsize(byte);
   showsize(bool);
   showsize(int);
   showsize(long);
   showsize(long long);
#endif

   // process command-line options

   if (argc == 1) {
      SayUsage (argv[0]);
      exit(4); }
   argno = HandleOptions (argc, argv);
   if (argno == 0) {
      fprintf (stderr, "\n*** No basefilename given\n\n");
      SayUsage (argv[0]);
      exit (4); }
   basefilename = argv[argno];

   // open the input file and create the working directory for output files

   strlcpy (filename, basefilename, MAXPATH);
   strlcat (filename, ".csv", MAXPATH);
   time_t start_time = time(NULL);
   if (!terse) {
      log("reading file \"");
      log(filename);
      log("\" on %s", ctime(&start_time)); }
   inf = fopen (filename, "r");
   assert(inf,"Unable to open input file");

   if(mkdir(basefilename)!=0) assert(errno==EEXIST || errno==0, "can't create directory");

   if (logging) { // Open the log file
      sprintf(logfilename, "%s\\%s.log", basefilename, basefilename);
      assert (logf = fopen (logfilename, "w"), "Unable to open log file"); }


   fgets(line, MAXLINE, inf); // first two lines in the input file are headers from Saleae
   //log("%s",line);
   fgets(line, MAXLINE, inf);
   //log("%s\n",line);

   while (1) { // for all lines of the file
      init_blockstate();  // initialize for a new block
      block.parmset = starting_parmset;
      blockstart = ftell(inf);// remember the file position for the start of a block
      dlog("\n*** block start file pos %'lld at %.7lf\n", blockstart, timenow);

      bool keep_trying;
      int last_parmset;
      block.tries=0;
      do { // keep reinterpreting a block with different parameters until we get a perfect block or we run out of parameter choices
         keep_trying=false;
         ++parmsets[block.parmset].tried;  // note that we used this parameter set in another attempt
         last_parmset = block.parmset;
         init_trackstate();
         dlog("\n     trying block %d with parmset %d at bytes %'lld at time %.7lf\n", numblks+1, block.parmset, blockstart, timenow);
         if (!readblock()) goto endfile; // ***** read a block ******
         struct results_t *result = &block.results[block.parmset];
         if (result->blktype == BS_NONE) goto endfile; // stuff at the end wasn't a real block
         ++block.tries;
         dlog("     block %d is type %d parmset %d, minlength %d, maxlength %d, %d parity errs, %d faked bits at %.7lf\n", //
              numblks+1, result->blktype, block.parmset, result->minbits, result->maxbits, result->parity_errs, result->faked_bits, timenow);
         if (result->blktype == BS_TAPEMARK) goto done;  // if we got a tapemake, we're done
         if (result->blktype == BS_BLOCK && result->parity_errs == 0 && result->faked_bits == 0) { // if we got a perfect block, we're done
            if (block.tries>1) ++numgoodmultipleblks;  // bragging rights; perfect blocks due to multiple parameter sets
            goto done; }
         if (MULTIPLE_TRIES && result->minbits != 0) { // if there are no dead tracks (which probably means we saw noise)
            int next_parmset=block.parmset; // then find another parameter set we haven't used yet
            do {
               if (++next_parmset >= MAXPARMSETS) next_parmset = 0; }
            while (next_parmset != block.parmset &&
                   (parmsets[next_parmset].clk_factor==0 || block.results[next_parmset].blktype!=BS_NONE));
            //log("found next parmset %d, block_parmset %d, keep_trying %d\n", next_parmset, block.parmset, keep_trying);
            if (next_parmset != block.parmset) { // we have a parmset, so can try again
               keep_trying = true;
               block.parmset = next_parmset;
               //log("   retrying block %d with parmset %d at bytes %'lld at time %.7lf\n", numblks+1, block.parmset, blockstart, timenow);
               assert(fseek(inf,blockstart,SEEK_SET)==0,"seek failed 1"); } } }
      while (keep_trying);

      // We didn't succeed in getting a perfect decoding of the block, so pick the best of the bad decodings.

      if (block.tries > 1) {
         dlog("looking for good parity blocks\n");
         int min_bad_bits=INT_MAX;
         for (int i=0; i<MAXPARMSETS; ++i) { // Try 1: find a decoding with good parity and the minimum number of faked bits
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->parity_errs==0 && result->faked_bits<min_bad_bits) {
               min_bad_bits = result->faked_bits;
               block.parmset = i;
               dlog("  best good parity choice is parmset %d\n", block.parmset); } }
         if (min_bad_bits < INT_MAX) goto done;

         dlog("looking for minimum bad parity blocks\n");
         min_bad_bits=INT_MAX;
         for (int i=0; i<MAXPARMSETS; ++i) { // Try 2: Find the decoding with the mininum number of parity errors
            struct results_t *result = &block.results[i];
            if (result->blktype == BS_BLOCK && result->parity_errs < min_bad_bits) {
               min_bad_bits = result->parity_errs;
               block.parmset = i;
               dlog("  best bad parity choice is parmset %d\n", block.parmset) ; } }
         if (min_bad_bits < INT_MAX) goto done;

         dlog("looking for least malformed blocks\n");
         int min_track_diff=INT_MAX;
         for (int i=0; i<MAXPARMSETS; ++i) { // Try 3: The block is malformed; find the decoding with the minimum difference in track lengths
            struct results_t *result = &block.results[i];
            int track_diff = result->maxbits - result->minbits;
            if (result->blktype == BS_MALFORMED && track_diff < min_track_diff) {
               min_track_diff = track_diff;
               block.parmset = i;
               dlog("  best malformed block choice is parmset %d\n", block.parmset); } }
         assert (min_track_diff < INT_MAX, "bad malformed block status"); }

done:
      dlog("  chose parmset %d as best after %d tries\n", block.parmset, block.tries);
      ++parmsets[block.parmset].chosen;  // count times that this parmset was chosen to be used
      if(block.tries>1 // if we processed the block multiple times
            && last_parmset != block.parmset) { // and the decoding we chose isn't the last one we did
         assert(fseek(inf,blockstart,SEEK_SET)==0,"seek failed 2"); // then reprocess the chosen one to retrieve the data
         dlog("     rereading parmset %d\n", block.parmset);
         init_trackstate();
         assert(readblock(), "got endfile rereading a block");
         struct results_t *result = &block.results[block.parmset];
         dlog("     reread block %d is type %d, minlength %d, maxlength %d, %d parity errs, %d faked bits at %.7lf\n", //
              numblks+1, result->blktype, result->minbits, result->maxbits, result->parity_errs, result->faked_bits, timenow); }
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
         fatal ("bad block state after decoding", ""); }

#if USE_ALL_PARMSETS
      do { // If we start with a new parm set each time, we'll use them all relatively equally and can see which ones are best
         if (++starting_parmset >= MAXPARMSETS) starting_parmset = 0;
         dir * }
      while (parmsets[starting_parmset].clk_factor==0);
#else
      // otherwise we always start with parmset 0, which has proven to be the best for most tapes
#endif
   }
endfile:
   close_file();

   //setlocale(LC_ALL,"");
   if (!terse) log("\n%'d samples processed in %l.0f seconds, created %d files\n" //
                      "detected %d tape marks, and %d data blocks of which %d had parity errors and %d were malformed.\n",//
                      lines_in, difftime(time(NULL),start_time), numfiles, //
                      numtapemarks, numblks, numbadparityblks, nummalformedblks);
   if (verbose && MULTIPLE_TRIES) {
      log("%d perfect blocks needed to try more than one parm set\n", numgoodmultipleblks);
      for (int i=0; i<MAXPARMSETS; ++i) //
         if (parmsets[i].tried > 0)
            log("parm set %d was tried %4d times and used %4d times, or %5.1f%%: clk factor %.1f, window %2d, move threshold %.2fV\n",//
                i, parmsets[i].tried, parmsets[i].chosen, 100.*parmsets[i].chosen/parmsets[i].tried,
                parmsets[i].clk_factor, parmsets[i].avg_window, parmsets[i].move_threshold); } }

//*
