// file: decoder.h

// header file for readtape

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <direct.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <inttypes.h>
typedef unsigned char byte;
#define NULLP (void *)0

#define NTRKS 9
#define MAXBLOCK 32768
#define MAXPATH 150

#define ERR_BLOCK_TOO_BIG 		0x01
#define ERR_TRACKS_INCONSISTENT	0x02
#define ERR_PREAMBLE_BAD		0x04
#define ERR_BAD_PARITY			0x08

struct sample_t {
	double time;
	float voltage[NTRKS];
};

void fatal(char *, char *);
void assert(bool, char *, char *);
void log(const char *, ...);
void init_trackstate(void);
void process_sample(struct sample_t *);
void got_tapemark(void);
void got_crap (int);
void got_datablock (uint16_t *, int, float);

