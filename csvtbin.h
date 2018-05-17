//file: csvtbin.h
/******************************************************************************

    format of the .tbin compressed analog magnetic tape data file
      
This describes a file format which is more general that what is implemented.
The current code supports only one data block with 16-bit non-delta samples.

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

#define TBIN_FILE_FORMAT 1

enum mode_t {
   UNKNOWN, PE, NRZI, GCR, ALL };

struct tbin_hdr_t {     // the file header for .tbin files, which appears once
   char tag[8];                     // a zero-terminated ASCII string identifier tag
#define HDR_TAG "TBINHDR"
   char descr[80];                  // tape description, a zero-terminated ASCII string
   union {
      struct { // everything below must be 4-byte numbers, stored little-endian
         uint32_t tbinhdrsize;      // size of this header in bytes
         uint32_t format;           // .tbin file format version
         struct tm time_written;    // when the analog tape data was written (9 integers)
         struct tm time_read;       // when the analog tape data was digitized (9 integers)
         struct tm time_converted;  // when the digitized data was converted to .tbin (9 integers)
         uint32_t options;          // file format options, TBINOPT_xxx
 #define TBINOPT_xxx 0x01           // options TBD
         uint32_t ntrks;            // number of tracks (heads)
         uint32_t tdelta;           // time between samples, in nanoseconds
         float maxvolts;            // maximum voltage for any sample
         enum mode_t mode;          // encoding mode (PE, NRZI, etc), if known
         float bpi;                 // data density in bits per inch, if known
         float ips;                 // read speed in inches per second, if known
      } s;
      uint32_t a[0];    // (for accessing the above as an array of 4-byte little-endian integers)
   } u; };

struct tbin_dat_t {     // the data header that starts each block of data
   char tag[4];                     // a zero-terminated ASCII string identifier tag
#define DAT_TAG "DAT"
   byte options;                    // data format options, TDATOPT_xxx
#define TDATOPT_deltas 0x01         // is each sample a delta from the previous sample?
   byte sample_bits;                // number of bits for each voltage sample
   byte rsvd1, rsvd2;               // reserved fields so that the next field is on an 8-byte boundary
   uint64_t tstart;                 // time of the next sample in nanoseconds, relative to the start of the tape
};
// What follows are multiple sets of "ntrks" packed little-endian signed integers,
// in the track (head) order msb..lsb and then parity, for each set.
// Each integer is "sample_bits" long, and encodes the read head voltage for a sample in the range
// -maxvolts..+maxvolts by -(2^sample_bits-1)-1..+2^(sample_bits-1)-1, rounded to the closest integer.
// (For sample_bits=16, that's -32767..+32767.)
// At the end is a single value -2^(sample_bits-1), which is outside that range.
// (For sample_bits=16, that's -32768, or 0x8000.)
// After that can (in theory) be more tbin_dat structures, or the end of the file.

// portability assumptions not otherwise explicit in the types:
//   enum is 4 bytes, and are sequentially numbered from 0
//   float is 4 bytes
//   datetime struct tm is nine 4-byte integers (36 bytes)
//   all numeric fields are little-endian

//*