#!/usr/bin/make -f

READTAPE ?= readtape

all:
test:
	$(READTAPE) -v -m -nrzi -ips=50 -deskew -ebcdic -linefeed -outp=results/ PLAGO_beginning.tbin
	$(READTAPE) -v -m -nrzi -hex            -ascii            -outp=results/ Microdata_20blks.tbin
	for excepcted in expected_results/*.{tap,bin}; do \
		[ -r "$${expected}" ] || continue; \
		created="results/$$(basename "$${expected}")"; \
		cmp "$${expected}" "$${created}" || { echo "$${expected} does not match $${created}." >&2; exit 1; }; \
	done

.PHONY: all test
