readtape -v -m -nrzi -ips=50 -deskew -ebcdic -linefeed -outp=results\ %1 %2 %3 PLAGO_beginning
readtape -v -m -nrzi -hex -ascii -outp=results\ %1 %2 %3 Microdata_20blks