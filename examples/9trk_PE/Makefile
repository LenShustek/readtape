#!/usr/bin/make -f

READTAPE ?= readtape

all:
test:
	$(READTAPE) -v -m -ntrks=9 -pe -bpi=1600 -ips=50 -order=01234576p -tap -ascii  -linefeed     -outp=results/ 1600bpi_ukn_6s.tbin
	$(READTAPE) -v -m -ntrks=9 -pe -bpi=1600 -ips=50                  -tap -ebcdic -linesize=137 -outp=results/ LJS009_part1_39blks.tbin
	for excepcted in expected_results/*.{tap,bin}; do \
		[ -r "$${expected}" ] || continue; \
		created="results/$$(basename "$${expected}")"; \
		cmp "$${expected}" "$${created}" || { echo "$${expected} does not match $${created}." >&2; exit 1; }; \
	done

.PHONY: all test
