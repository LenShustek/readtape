## Performance analysis

I compared analysis of sampled analog data using the traditional readtape
csv-to-tbin conversion process, and analysis on the native Saleae binary export
format.

As a worst-case scenario, I captured data from a full-size reel (3600') of
9-track tape, using a Control Data 92181 transport at 25ips, and a Saleae Logic
Pro 16. The transport was placed in diagnostic test mode to advance the
transport forward under local control. 25ips was chosen over 100ips for archival
work as being less stressful to the source material, and because 50ips is not a
supported speed. The Logic Pro was connected to the output of the third-stage
amplifiers in the front end, just prior to the comparator that generates the
digital signal. Saleae's Logic software (v2.4.44) captured data at 3.125MS/s and
was saved to disk in the software's native .sal file format.

All capture and analysis was performed with a Dell PowerEdge R630, with the
following hardware specifications: dual Xeon E5-2667v3 CPUs, each 8C/16T @
3.2GHz, 256GB RAM, and a Seagate Nytro 3.84TB 12Gbps SAS-3 solid state drive for
data storage. This configuration is about US$2k at current market prices (1 May
2026). I am fairly certain that CPU performance is the primary bottleneck in
this setup, given the single-threaded readtape workload.

For the traditional readtape workflow, I exported this capture first to CSV
form, then converted to tbin using the csvtbin program, and then finally
analysed using readtape itself. For the revised workflow, I exported the capture
to Saleae native binary form, the metadata file (meta.json) was extracted from
the .sal file using `unzip capture.sal meta.json`, and then the result was
analysed using readtape.

The results are best summarised in a table:

| Step name                |   Traditional time | New workflow time | Speedup  |
|:-------------------------|-------------------:|------------------:|---------:|
| Load .sal in Logic       |       17m 45s      |     17m 45s       |   1.00x  |
| Export from Logic        |     4h 8m 59s      |      2m 32s       |   98.3x  |
| Conversion (.tbin only)  |     1h 0m 21s      |          0s       |       ∞  |
| readtape analysis        |    3h 10m 47s      |  2h 57m 32s       |   1.07x  |
| **Total analysis job time** | **8h 37m 52s**  |    **3h 17m 49s** | **2.62x**|

I believe the revised workflow is slightly faster even during readtape execution
due to the lack of required conversion between 16-bit integers and single-
precision floats. It surprised me that it was as much as 5-7% faster!

Of further interest is the size of the file(s) on disk:

| File type                | Size in bytes   | Compared to original file size |
|--------------------------|----------------:|-------------------------------:|
| capture.sal              |  13,842,405,385 |                          1.00  |
| capture.csv              | 262,380,121,131 |                         18.95x |
| capture.tbin             |  63,214,852,290 |                          4.57x |
| analog_*.bin + meta.json | 126,834,709,441 |                          9.16x |

While the extracted binary files are about double the size of the TBIN file,
they are in turn half the size of the CSV file. Further, there is ample reason
to simply retain the original .sal file instead, which is several times smaller
than both the TBIN and exported bin data due to the use of internal run-length
encoding, per a post by one of their engineers on their support forum.

Finally, I compressed the readtape input files (tbin, and the extracted .bin
files + meta.json), using multithreaded 7-Zip 25.01 (`7z a -mmt32`):

| File type                | Size in bytes   | Time to compress | Compared to original file size |
|--------------------------|----------------:|-----------------:|-------------------------------:|
| capture.sal              |  13,842,405,385 |      _n/a_       |                          100%  |
| analog*.bin+meta.json.7z |   4,395,210,615 |    00:11:03      |                         31.7%  |
| capture.tbin.7z          |   9,634,232,733 |    00:16:22      |                         69.6%  |

If absolute smallest archival file size is desired, it makes sense to compress
the analog*bin and meta.json files after export and retain those instead of the
.sal file. My guess is that its per-track files benefit more from compression
than the combined file, as these files compressed by 94-96% vs. only 70% for the
tbin file. Faster compression is possible using parallelisable tools such as
7zip or lbzip2.

In conclusion, the direct processing of data in native binary form is not only
faster (by eliminating the costly CSV export and TBIN conversion steps), but
also requires less disk space. Retaining the original .sal file also allows for
storage of metadata (ips, bpi, date/time stamps, in-capture timestamping, etc.)
alongside binary capture in a single file and is easily exported using any ZIP
utility. One negative tradeoff is the reliance on the closed-source Saleae Logic
program and its supported operating system (Windows, Mac, Linux) for continued
access to the binary data, as the .sal binary compression algorithm is not
documented, nor is the means of applying the analog calibration data stored in
the meta.json file.
