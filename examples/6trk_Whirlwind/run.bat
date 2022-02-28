readtape -whirlwind -v3 -fluxdir=auto -tap -deskew -octal2 -flexo -outp=results\ %1 %2 %3 132_pt1
comp /M expected_results\*.tap results
@echo off
if %ERRORLEVEL% NEQ 0 pause .tap files differ
