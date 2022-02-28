/***************************************************************************************************

    mag tape reader

    Digitize the analog magnetic waveforms on old computer tapes
    for subsequent software processing and extraction of the data.

    This runs on a Teensy 3.6 32-bit ARM processor board,
    which interfaces to an analog data acquisition system
    based on a Wolfson/Cirrus WM8235 9-channel A-to-D converter.

   ------------------------------------------------------------------------------------------------------
   Copyright (c) 2017, Len Shustek

                                    The MIT License (MIT)

   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
   associated documentation files (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge, publish, distribute,
   sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all copies or
   substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
   NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
   ------------------------------------------------------------------------------------------------------

  **** Change log ****

    4 Nov 2017, L. Shustek, First sketches

*****************************************************************************************************/
#define VERSION "0.1"
#define DEBUG false

#define MCLK_FREQ 5000000 // 5 Mhz min spec, but can test as low as 3 Mhz
//#define DIVIDE_BY 4094    // maximum possible 12-bit number relatively prime to 9, for slowest data rate!
#define DIVIDE_BY 2000

#define USEC_PER_SAMPLE ((float)DIVIDE_BY/9/(MCLK_FREQ/1E6))
// at 5 Mhz MCLK, 45 Mhz / 4094 = 10.9916951 Khz, or 90.9777777 usec/sample
// 4096 samples/buffer x 90.9777777 usec = 0.372645 seconds to fill a buffer

#include <Arduino.h>
#include <SPI.h>

//******* hardware configuration

#define MCLK        20  // the 5 Mhz input clock to the WM8235 analog converter

#define SIO_ENB     30  // serial I/O enable, active low
#define SIO_CLK     14  // serial I/O clock (SCK0)
#define SIO_DOUT     7  // serial I/O data out to WM8235 (MOSI0)
#define SIO_DIN      8  // serial I/O data in from WM8235 (MISO0)

#define CNTR_LOAD   34  // divide-by-N counter load at next TGSYNC pulse, active low
#define CNTR_LD0    16  // divide-by-N counter preload value: 4096 - divisor
#define CNTR_LD1    17
#define CNTR_LD2    18
#define CNTR_LD3    19
#define CNTR_LD4    21
#define CNTR_LD5    24
#define CNTR_LD6    25
#define CNTR_LD7    26
#define CNTR_LD8    27
#define CNTR_LD9    28
#define CNTR_LD10   29
#define CNTR_LD11   31

#define BUTTON1     5   // pushbuttons
#define BUTTON2     6
#define LCD_SDA     4   // LCD data
#define LCD_SCL     3   // LCD clock
#define ANALOG_OUT  A21 // test analog output pin

#define SAMPLE_IN   GPIOC_PDOR  // register C: 0..9 is digitized sample, 10 is TGSYNC, 11 is DMA request
//.                                (bits 12..31 don't come off the chip and are unused)
#define SAMPLE_DMA  38          // register C bit 11 is DMA request

byte output_pins[] = {SIO_ENB, CNTR_LOAD, LCD_SDA, LCD_SCL, /*ANALOG_OUT,*/ 0 };
byte counter_load_pins[] = {CNTR_LD0, CNTR_LD1, CNTR_LD2, CNTR_LD3, CNTR_LD4, CNTR_LD5,
                            CNTR_LD6, CNTR_LD7, CNTR_LD8, CNTR_LD9, CNTR_LD10, CNTR_LD11, 0 };
byte input_pins[] = {BUTTON1, BUTTON2, 0 };
byte portc_input_pins[] = {15, 22, 23, 9, 10, 13, 11, 12, 35, 36, 37, 38, 0 }; // Port C bits 0..11

void fatal_err(const char *msg) {
   Serial.println(msg);
   while (1) ; }
void assert (bool condition, const char *msg) {
   if (!condition) fatal_err (msg); }

#define STRINGIFY2(X) #X
#define STRINGIFY(X) STRINGIFY2(X)

//********* buffer and DMA stuff

#define NBUFS   4       // number of buffers
#define NSAMPS 4096     // number of 16-bit samples per buffer

struct __attribute__((packed)) TCD_t { // DMA Transfer Control Descriptor
   void *SADDR;       // source address
   int16_t SOFF;      // source address offset (increment)
   uint16_t ATTR;     // transfer attributes
   uint32_t NBYTES;   // number of bytes per request
   int32_t SLAST;     // source address adjustment after last transfer
   uint16_t *DADDR;   // destination address points to our buffer
   int16_t DOFF;      // destination address offset (increment)
   uint16_t CITER;    // current iteration count
   struct TCD_t *SGA; // scatter/gather: address of next TCD
   uint16_t CSR;      // control and status register
   uint16_t BITER;    // beginning iteration count
} *TCDs;  // ptr to array of NBUFS TCDs, aligned on a 32-byte boundary


#define NUM_HISTORY 100   // number of buffer history records we save
struct {
   unsigned long buftime; // how long it took to fill this buffer
} bufhistory[NUM_HISTORY];
volatile int numbufs_filled = 0, bufhistory_ndx = 0;

void dumptcd(const char *msg) {
   Serial.print("****TCD "); Serial.println(msg);
   volatile uint32_t * volatile ptr = (volatile uint32_t * volatile) 0x40009040; // TCD2
   Serial.print("SADDR: "); Serial.println(*ptr++, HEX);
   Serial.print("SSIZES, SOFF: "); Serial.println(*ptr++, HEX);
   Serial.print("NBYTES: "); Serial.println(*ptr++, HEX);
   Serial.print("SLAST: "); Serial.println(*ptr++, HEX);
   Serial.print("DADDR: "); Serial.println (*ptr++, HEX);
   Serial.print("CITER, DOFF: "); Serial.println(*ptr++, HEX);
   Serial.print("SGA: "); Serial.println(*ptr++, HEX);
   Serial.print("BITER, CSR: "); Serial.println(*ptr++, HEX);
   Serial.print("DMA_ES: "); Serial.println(DMA_ES, HEX); }

void init_buffers(void) {
#define DMACHAN 2    // use DMA channel 2 to avoid possible use of 0/1 by SDcard routines
   //.               // (But lots of other constants need to change if you change this!)
   SIM_SCGC7 |= SIM_SCGC7_DMA;    // enable DMA MUX clock
   SIM_SCGC6 |= SIM_SCGC6_DMAMUX; // enable DMA module clock
   DMA_CERQ = DMACHAN;  // clear out DMA channel
   DMA_CERR = DMACHAN;
   DMA_CEEI = DMACHAN;
   DMA_CINT = DMACHAN;
   // Allocate an array of DMA TCDs ("Transfer Control Descriptors") on a 32-byte boundary.
   // They are linked to each other in a circular ring, and are used by the "scatter"
   // DMA mode to fill in the buffers they point to.
   assert (sizeof(struct TCD_t) == 32, "bad TCD struct");
   TCDs = (struct TCD_t *) (((uint32_t) malloc(32 + NBUFS * sizeof(struct TCD_t)) + 31) & 0xffffffe0UL);
   for (int bufno = 0; bufno < NBUFS; ++bufno) {
      Serial.print("---initializing TCD at "); Serial.println((uint32_t)&TCDs[bufno], HEX);
      TCDs[bufno].SADDR = (void *)&GPIOC_PDIR; // input is from port C, lower 16 bits only
      //N.B.: We assume the processor is running in little-endian mode
      Serial.print("TCDs[bufno].SADDR: "); Serial.println((uint32_t) TCDs[bufno].SADDR, HEX);
      TCDs[bufno].SOFF = 0;            // zero increment to repeatedly read from the same register
      TCDs[bufno].ATTR = 0x0101;       // 16-bit source and  destination transfer size
      TCDs[bufno].NBYTES = 2;          // 2 bytes per DMA request (minor loop mapping disabled)
      TCDs[bufno].SLAST = 0;           // last source address adjustment: stays on the register
      TCDs[bufno].DADDR = (uint16_t *)(malloc(NSAMPS * 2)); // destination addr points to a buffre
      assert (TCDs[bufno].DADDR, "mem error"); // out of memory for buffers
      Serial.print("TCDs[bufno].DADDR: "); Serial.println((uint32_t) TCDs[bufno].DADDR, HEX);
      for (int i = 0; i < NSAMPS; ++i) // fill buffer with recognizable stuff, for debugging
         TCDs[bufno].DADDR[i] = bufno * 10000 + i;
      TCDs[bufno].DOFF = 2;            // increment for each transfer
      TCDs[bufno].CITER = NSAMPS;      // no channel linking after minor loop; current major count
      TCDs[bufno].SGA = &TCDs[bufno == NBUFS - 1 ? 0 : bufno + 1]; // address of next TCD to fetch
      Serial.print("TCDs[bufno].SGA: "); Serial.println((uint32_t)TCDs[bufno].SGA, HEX);
      TCDs[bufno].CSR = 0x12;          //  enable scatter; enable interrupt on major count complete
      TCDs[bufno].BITER = NSAMPS;      // no channel linking after minor loop; beginning major count
      Serial.print("TCDs[bufno].CITER: "); Serial.println(TCDs[bufno].CITER, HEX);
      Serial.print("TCDs[bufno].BITER: "); Serial.println(TCDs[bufno].BITER, HEX); }

   // copy the first TCD to the DMA engine hardware
   for (int i = 0; i < 8; ++i) // copy 32 bytes as 8 4-byte integers
      ((volatile uint32_t * volatile)&DMA_TCD2_SADDR)[i] = ((volatile uint32_t * volatile)&TCDs[0])[i];
   Serial.print("DMA_TCD2_SADDR: "); Serial.println((uint32_t) DMA_TCD2_SADDR, HEX);
   Serial.print("DMA_TCD2_DADDR: "); Serial.println((uint32_t) DMA_TCD2_DADDR, HEX);
   dumptcd("after initializing hardware registers");
   DMAMUX0_CHCFG2 = DMAMUX_SOURCE_PORTC | DMAMUX_ENABLE;  // the DMA request source is Port C
   NVIC_ENABLE_IRQ(IRQ_DMA_CH2);  // enable the DMA interrupt
}

volatile unsigned long time_bufstart;

void dma_ch2_isr(void) {  // the DMA buffer complete interrupt
   unsigned long time_bufend;
   time_bufend = micros(); // record how long it took
   bufhistory[bufhistory_ndx].buftime = time_bufend - time_bufstart;
   time_bufstart = time_bufend;
   ++numbufs_filled;
   if (++bufhistory_ndx >= NUM_HISTORY) bufhistory_ndx = 0;
   DMA_CINT = DMACHAN;  // clear the interrupt bit
}

//********   generate an analog output test signal

void do_analog_output(void) {
   static IntervalTimer mytimer;
   analogReference(1); // 1.2V ref, not 3.3V
   mytimer.begin(timerint, 200);  // timer interrupt in microseconds
}
void timerint(void) {
   static int analogval = 0;
   analogval += 1;
   if (analogval > 255) analogval = 0;
   analogWrite(ANALOG_OUT, analogval); }

//******* register access routines for DM8235

SPISettings SIO_settings(1000000, MSBFIRST, SPI_MODE0); // sample on rising edge, change on falling edge

void WM_writereg(uint16_t reg, byte data) {
   SPI.beginTransaction(SIO_settings);
   digitalWrite(SIO_ENB, LOW);
   SPI.transfer(reg >> 8); // all tranfers are 8 bits
   SPI.transfer(reg & 0xff);
   SPI.transfer(data);
   delayMicroseconds(2);
   digitalWrite(SIO_ENB, HIGH); // transfer to internal register
   SPI.endTransaction(); }

byte WM_readreg(int reg) {
   uint16_t data;
   SPI.beginTransaction(SIO_settings);
   digitalWrite(SIO_ENB, LOW);
   SPI.transfer((reg >> 8) | 0x80);
   SPI.transfer(reg & 0xff);
   data = SPI.transfer(0);
   delayMicroseconds(2);
   digitalWrite(SIO_ENB, HIGH);
   SPI.endTransaction();
   return data; }

void WM_showreg(int regno)   {
   Serial.print(regno, HEX); Serial.print(": ");
   Serial.println(WM_readreg(regno), HEX); }

//********  initialization

void divide_by_N (uint16_t n) {   // set up divide-by-N counter divisor
   uint16_t mask = 4096 - n; // 2's complement to load with
   for (int i = 0; i < 12; ++i) {
      digitalWrite(counter_load_pins[i], mask & 1);
      mask >>= 1; } }

void setpins (byte pinarray[], int type) { // set I/O pin modes
   int i = 0;
   while (pinarray[i] != 0)
      pinMode(pinarray[i++], type); }

void setup(void) {
   while (!Serial) ;  // require the serial monitor, at the moment
   delay(2000);
   Serial.begin(115200);
   Serial.println("starting magtape_reader");
   Serial.print("F_CPU "); Serial.println(F_CPU / 1000000);
   Serial.print("F_PLL "); Serial.println(F_PLL / 1000000);
   Serial.print("F_BUS "); Serial.println(F_BUS / 1000000);

   setpins (portc_input_pins, INPUT);  // configure I/O pins
   setpins (input_pins, INPUT_PULLUP);
   setpins (counter_load_pins, OUTPUT);
   digitalWrite(CNTR_LOAD, HIGH);  // let the counter free run
   setpins (output_pins, OUTPUT);

   // output 5 Mhz square wave on FTM0 channel 5, PTD5, pin 20 on the board
   Serial.println("starting mclk");
   analogWriteFrequency(MCLK, MCLK_FREQ);
   analogWrite(MCLK, 128);

   // start analog test output
   Serial.println("starting analog test signal");
   do_analog_output();

   // configure the WM8235 analog signal digitizer
   digitalWrite(SIO_ENB, HIGH);
   SPI.setMOSI(SIO_DOUT);
   SPI.setMISO(SIO_DIN);
   SPI.setSCK(SIO_CLK);
   SPI.begin();

   WM_writereg(0x1c, 0x10); // PLL divider 1: EX DIV ratio 2
   WM_writereg(0x80, 0x20); // DLL config 1: DLGAIN 1 0 (default); generate TG and AFE clocks
   WM_writereg(0x03, 0x03); // power down (system reset)
   delay(2);
   WM_writereg(0x03, 0x00); // power up
   delay(2);
   WM_writereg(0x04, 0x60); // AVDD for VRLC DAC top, positive PGA input, 1.8V full scale ADC, line clamp, non-CDS (S/H) mode
   WM_writereg(0x05, 0x13); // bypass level shift of VRLC follower, enable control source followers (voltage buffers)
   WM_writereg(0x06, 0x0a); // disble VRLC DAC (used for S/H operation); VRLC/VBIAS is grounded
   WM_writereg(0x07, 0xa0); // 0xa0: output enable, CMOS output mode  (use 0xe0 to force all data outputs to zero)
   WM_writereg(0x0d, 0x05); // 5ma output drive, controlled by OP_DRV, for all CMOS digital outputs
   WM_writereg(0x19, 5);    // monitor output 4=RSMP, 5=VSMP, 6=ACLK, 7=OCLK
   WM_writereg(0xa0, 0x03); // TG config 1: master mode, TG enabled
   WM_writereg(0xa1, 20);    // TG config 2: line length in pixels (8 LSBs) 20+1 5Mhz cycles = 4.2 usec per TGSYNC
   WM_writereg(0xa2, 0);    // TG config 3: line length in pixels (7 MSBs)
   WM_writereg(0xa5, 0x00); // TG config 6: disable all CLKn outputs to save power
   WM_writereg(0xa9, 0x00); // TG config 10: disable all CLKn outputs
   WM_writereg(0x03, 0x03); // power down (system reset)
   delay(2);
   WM_writereg(0x03, 0x00); // power up
   delay(2);

   for (int regno = 0; regno <= 31; ++regno) // display the first few config registers
      WM_showreg(regno);
   for (int regno = 0x82; regno <= 0x85; ++regno) // RSMP and VSMP edge timing
      WM_showreg(regno);

   PORTC_PCR11 =  // port C bit 11 (pin 38) pin control
      PORT_PCR_IRQC(2)    // ISF flag and DMA request on falling edge
      | PORT_PCR_MUX(1);  // pin mux control: Alternative 1 (GPIO PTC11)
   Serial.println("DMA request set up");
   init_buffers();        // setup buffers and DMA
   divide_by_N (DIVIDE_BY);       // setup divisor
   digitalWrite(CNTR_LOAD, LOW); // load divide-by-N counter at TGSYNC pulses
   delayMicroseconds (100);       // make sure we've seen TGSYNC pulses
   digitalWrite(CNTR_LOAD, HIGH);  // release the counter to free run
   Serial.println("counter running");
   Serial.print("DMA_ERR: "); Serial.println(DMA_ERR, HEX); // any errors?
   Serial.print("DMA_ES : "); Serial.println(DMA_ES, HEX);
   delay(100); // let everything run for a while
   #if 0
   Serial.println("****TCD[0]");
   for (int i = 0; i < 8; ++i) // display 32 bytes of first stored TCD as 8 4-byte integers
      Serial.println (((uint32_t *)TCDs)[i], HEX);
   for (int i = 0; i < NSAMPS; ++i) { // dump an initial buffer
      Serial.print(i); Serial.print(", ");
      Serial.print(i % 9); Serial.print(", ");
      Serial.println(TCDs[1].DADDR[i]); }
   #endif
   time_bufstart = micros();
   DMA_SERQ = DMACHAN; // enable requests on the DMA channel
}

void loop(void) {
   #if 0 // test with low four bits of the data register connected to LSB counter presets, and latch generated from software
#define STROBE 39
   pinMode(STROBE, OUTPUT);
   digitalWrite(STROBE, LOW);
   Serial.println("*** start loop");
   delay(100);
   while (numbufs_filled < 10) {
      static uint8_t val = 0; // HACK
      uint8_t mask;
      mask = val++;
      for (int i = 0; i < 4; ++i) {
         digitalWrite(counter_load_pins[i], mask & 1);
         mask >>= 1; }
      digitalWrite(STROBE, HIGH);
      delayMicroseconds(1);
      digitalWrite(STROBE, LOW);
      delayMicroseconds(90); // simulate 4095 45 Mhz countdown
   }
   #endif

   while (numbufs_filled < 25) ; // wait for some cycling around
   DMA_CERQ = DMACHAN; // stop requests on the DMA channel
   //WM_writereg(0xa0, 0x02); // stop digitizing: TG config 1: master mode, TG disabled
   Serial.println("done");
   Serial.print("DMA_ES : "); Serial.println(DMA_ES, HEX);
   Serial.print("DMA_TCD2_DADDR:"); Serial.print((uint32_t)DMA_TCD2_DADDR, HEX);
   Serial.print(" DMA_TCD2_CSR:"); Serial.println(DMA_TCD2_CSR, HEX);
   Serial.println();

   Serial.print(USEC_PER_SAMPLE); Serial.println(" usec per sample");
   for (int i = 0; i < bufhistory_ndx; ++i) {
      Serial.print("buffer fill in "); Serial.print(bufhistory[i].buftime); Serial.print(" usec, ");
      Serial.print( (float)bufhistory[i].buftime / USEC_PER_SAMPLE); Serial.println(" sample times"); }

   for (int i = 0; i < NSAMPS; ++i) { // dump buffer 1
      Serial.print(i); Serial.print(", ");
      Serial.print(i % 9); Serial.print(", ");
      //Serial.println(TCDs[1].DADDR[i] & 0x0f); // HACK: lower four bits
      Serial.print(TCDs[1].DADDR[i] >> 10); Serial.print(", ");
      Serial.println(TCDs[1].DADDR[i] & 0x3ff); }
   while (1) ; // hang
}
//*

