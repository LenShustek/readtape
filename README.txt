This is an experiment in recovering data from old magnetic computer 
tapes by recording the analog waveform and using software to decode the 
bits and reconstruct the original data. The objective is to correctly 
read tapes that have degraded beyond the point where conventional tape 
drives will work. 

I am currently recording data using a 16-channel Saleae logic analyzer
https://www.saleae.com connected to a Qualstar 1052 tape transport,
http://bitsavers.com/pdf/qualstar/500150B_1052serviceMan.pdf.
The input is taken from the output of the differential amplifier that
produces a +- 2V analog signal (for good tapes), before their thresholding
logic tries to decode it into bits.

(I had also designed and prototyped a custom 9-channel A-to-D converter
using the amazing Cirrus/Wolfson WM8235 9-channel analog front end, 
http://www.mouser.com/ds/2/76/WM8235_v4.2-532446.pdf, but I have so 
far been unable to find the right setting for their 300 configuration
registers that makes it work as a normal A-to-D converter. I may 
eventually get back to it.)

The data exported by the logic analyzer is a comma-separated-value file
whose lines contains a timestamp and the voltage of all nine read heads.
I'm currently sampling at 1.56 MS/s, which generates about 20 samples per
bit, since 1600 BPI x 50 IPS = 80,0000 bits/sec. I might be able to go 
down to the next lower sample rate of 781 kS/s. 

I've just started trying real tapes, but I have had good success reading
standard-labeled IBM tapes that the tape drive's electronics would not
have been able to cope with. Stay tuned for more reports. There are many
opportunities to improve the decoding algorithms. 

Now that the basic mechanism works, it should be possible to adapt this
for other media: 5- and 7-track tapes, other densities, and other encoding
schemes. 

Thanks to: Paul Pierce for the original inspiration, Al Kossow for the
tape drive and for suggesting Saleae, and Grant Saviers for detailed
consulting on the nitty-gritties. 