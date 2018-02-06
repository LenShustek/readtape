//file: readtape.c
/**********************************************************************

Read an IBM formatted 1600 BPI 9-track labelled tape
and create one disk file for each tape file.

The input is a CSV file with 10 columns: a timestamp in seconds,
and then the read head voltage for 9 tracks in the order
MSB...LSB, parity. The first two rows are text column headings.

***********************************************************************
Copyright (C) 2018, Len Shustek

---CHANGE LOG

20 Jan 2018, L. Shustek, started first version
05 Feb 2018, L. Shustek, First github posting, since it kinda works.

***********************************************************************
The MIT License (MIT): Permission is hereby granted, free of charge,
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
#define MAXLINE 400


// IBM standard labeled tape headers
// all fields not reserved are in EBCDIC characters

struct IBM_vol_t {
	char id[4]; 		// "VOL1"
	char serno[6];		// volume serial number
	char rsvd1[31];
	char owner[10];		// owner in EBCDIC
	char rxvd2[29];
};
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
	char rsvd1[7];
};
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
	char rsvd2[41];
};


FILE *inf,*outf, *logf;
char *basefilename;
int numfiles=0, numblks=0, numbadblks=0;
int numfilebytes;
bool logging=false;
extern double timenow;

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
	/* 78*/	 '?',  '`',  ':',  '#',  '|',  '\'',  '=',	'"',
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
	/* e0*/	 '\\',	'?',  'S',	'T',  'U',	'V',  'W',	'X',
	/* e8*/	 'Y',  'Z',  '?',  '?',  '?',  '?',  '?',  '?',
	/* f0*/	 '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',
	/* f8*/	 '8',  '9',  '?',  '?',  '?',  '?',  '?',  ' '
};

void log(const char* format,...) {
	va_list argptr;
	va_start(argptr, format);
	vfprintf(stdout, format, argptr);
	va_end(argptr);
	if (logging && logf) {
		va_start(argptr, format);
		vfprintf(logf, format, argptr);
		va_end(argptr);
	}
}


/**************  command-line processing  *******************/

void SayUsage (char *programName) {
	static char *usage[] = {
		"Use: readtape <options> <basefilename>",
		"   input file will be <basefilename>.csv",
		"   output files will be in the created directory <basefilename>\\",
		"   log file will also be there, as <basefilename>.log",
		"Options:",
		"  -l   create log file",
		NULL
	};
	int i = 0;
	while (usage[i] != NULL) fprintf (stderr, "%s\n", usage[i++]);
}

int HandleOptions (int argc, char *argv[]) {
	/* returns the index of the first argument that is not an option; i.e.
	does not start with a dash or a slash*/

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
				/* add more  option switches here */
opterror:
			default:
				fprintf (stderr, "\n*** unknown option:	%s\n\n", argv[i]);
				SayUsage (argv[0]);
				exit (4);
			}
		}
		else {
			firstnonoption = i;
			break;
		}
	}
	return firstnonoption;
}


bool compare4(uint16_t *d, char *c) { // string compare ASCII to EBCDIC
	for (int i=0; i<4; ++i)
		if (EBCDIC[d[i]>>1] != c[i]) return false;
	return true;
}

void copy_EBCDIC (byte *to, uint16_t *from, int len) { // copy and translate to ASCII
	while (--len >= 0)	to[len] = EBCDIC[from[len]>>1];
}


byte parity (uint16_t val) {
	byte p = val & 1;
	while (val >>= 1) p ^= val & 1;
	return p;
}

int count_parity_errs (uint16_t *data, int len) { // count parity errors
	int parity_errs = 0;
	for (int i=0; i<len; ++i) {
		if (parity(data[i]) != 1) ++parity_errs;
	}
	return parity_errs;
}

void dumpdata (uint16_t *data, int len) {
	log("block length %d\n", len);
	for (int i=0; i<len; ++i) {
		log("%02X ", data[i]>>1);
		if (i%16 == 15) {
			log(" ");
			for (int j=i-15; j<=i; ++j) log("%c", EBCDIC[data[j]>>1]);
			log("\n");
		}
	}
	if (len%16 != 0) log("\n");
}

void got_tapemark(void) {
	log("\n*** tapemark\n");
}

void close_file(void) {
	if(outf) {
		fclose(outf);
		log("file closed, %d bytes written\n", numfilebytes);
		outf = NULLP;
	}
}

void createfile(char *name){
	strcat(name, ".bin");
	log("\ncreating file \"%s\"\n", name);
	outf = fopen(name, "wb");
	assert (outf, "file create failed", "");
	numfilebytes = 0;
}

void got_datablock(uint16_t *data, int length, float avg_bit_spacing) {
	int parity_errors = count_parity_errs(data, length);

	if (length==80 && compare4(data,"VOL1")) { // IBM volume header
		struct IBM_vol_t hdr;
		copy_EBCDIC((byte *)&hdr, data, 80);
		log("\n*** label %.4s:	%.6s, owner %.10s\n", hdr.id, hdr.serno, hdr.owner);
		if (parity_errors) log("--> %d parity errors\n", parity_errors);
		//dumpdata(data, length);
	}
	else if (length==80 && (compare4(data,"HDR1") || compare4(data,"EOF1") || compare4(data,"EOV1"))){
		struct IBM_hdr1_t hdr;
		copy_EBCDIC((byte *)&hdr, data, 80);
		log("\n*** label %.4s:	%.17s, serno %.6s, created%.6s\n", hdr.id, hdr.dsid, hdr.serno, hdr.created);
		log("    volume %.4s, dataset %.4s\n", hdr.volseqno, hdr.dsseqno);
		if (compare4(data,"EOF1")) log("    block count:	%.6s, system %.13s\n", hdr.blkcnt, hdr.syscode);
		if (parity_errors) log("--> %d parity errors\n", parity_errors);
		//dumpdata(data, length);
		if (compare4(data,"HDR1")) { // create the output file from the name in the HDR1 label
			char filename[MAXPATH];
			int i;
			sprintf(filename, "%s\\%03d-%.17s%c", basefilename, numfiles++, hdr.dsid, '\0');
			for (int i=strlen(filename); filename[i-1]==' '; --i) filename[i-1]=0;
			createfile(filename);
		}
		if (compare4(data,"EOF1")) close_file();


	}
	else if (length==80 && (compare4(data,"HDR2") || compare4(data,"EOF2") || compare4(data, "EOV2"))) {
		struct IBM_hdr2_t hdr;
		;
		copy_EBCDIC((byte *)&hdr, data, 80);
		log("\n*** label %.4s:	RECFM=%.1s%.1s, BLKSIZE=%.5s, LRECL=%.5s\n", hdr.id, hdr.recfm, hdr.blkattrib, hdr.blklen, hdr.reclen);
		log("    job: %.17s\n", hdr.job);
		if (parity_errors) log("--> %d parity errors\n", parity_errors);
		//dumpdata(data, length);
	}
	else { // normal good data block
		//dumpdata(data, length);
		if(!outf) { // create a generate data file if we didn't see a file header label
			char filename[MAXPATH];
			sprintf(filename, "%s\\%03d", basefilename, numfiles++);
			createfile(filename);
		}
		for (int i=0; i<length; ++i) { // discard parity and write all the data bits
			byte b = data[i]>>1;
			assert(fwrite(&b, 1, 1, outf) == 1, "write failed", "");
		}
		numfilebytes += length;
		++numblks;
		log("wrote block %d, %d bytes, %d parity errors, bit spacing %.2f usec, at time %.7lf\n", numblks, length, parity_errors, avg_bit_spacing, timenow);
	}
};

void got_crap(int length) {
	++numblks;
	++numbadblks;
	log("bad block %d of length %d at time %.7lf\n", numblks, length, timenow);
}


void main(int argc, char *argv[]) {

	int argno;
	char filename[MAXPATH], logfilename[MAXPATH];
	char line[MAXLINE+1];
	int items, lines_in=0, lines_out=0;
	struct sample_t sample;

	assert(sizeof(struct IBM_vol_t)==80, "bad vol type", "");
	assert(sizeof(struct IBM_hdr1_t)==80, "bad hdr1 type", "");
	assert(sizeof(struct IBM_hdr2_t)==80, "bad hdr2 type", "");

	// process options

	if (argc == 1) {
		SayUsage (argv[0]);
		exit(4);
	}
	argno = HandleOptions (argc, argv);
	if (argno == 0) {
		fprintf (stderr, "\n*** No basefilename given\n\n");
		SayUsage (argv[0]);
		exit (4);
	}
	basefilename = argv[argno];


	// open the input file and create the working directory

	strlcpy (filename, basefilename, MAXPATH);
	strlcat (filename, ".csv", MAXPATH);
	inf = fopen (filename, "r");
	assert(inf,"Unable to open input file:", filename);

	if (mkdir(basefilename)!=0) assert(errno==EEXIST, "can't create directory ",basefilename);

	if (logging) { // Open the log file
		sprintf(logfilename, "%s\\%s.log", basefilename, basefilename);
		assert (logf = fopen (logfilename, "w"), "Unable to open log file", logfilename);
	}
	log("reading file %s\n", filename);
	fgets(line, MAXLINE, inf); // first two lines are headers
	log("%s",line);
	fgets(line, MAXLINE, inf);
	log("%s\n",line);

	init_trackstate(); // initialize for a new block

	while (1) { // read the whole CSV file
		if (!fgets(line, MAXLINE, inf)) break;
		line[MAXLINE-1]=0;
		items = sscanf(line, " %lf, %f, %f, %f, %f, %f, %f, %f, %f, %f ", &sample.time,
			&sample.voltage[0], &sample.voltage[1], &sample.voltage[2],
			&sample.voltage[3], &sample.voltage[4], &sample.voltage[5],
			&sample.voltage[6], &sample.voltage[7], &sample.voltage[8]);
		assert (items == NTRKS+1,"bad CSV line format", "");
		++lines_in;
		process_sample(&sample);
	}
	close_file();
	log("\nall samples processed, created %d files, processed %d data blocks of which %d were bad.\n", numfiles, numblks, numbadblks);
}


//*
