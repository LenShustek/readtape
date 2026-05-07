#!/usr/bin/make -f
# On Linux/macOS, override LOGIC with correct path if necessary
# On Windows, override LOGIC="" or correct path

READTAPE ?= readtape
LOGIC ?= /usr/local/bin/Logic
SALEXTRACT ?= sal-extract -a $(LOGIC)

all:
test: TestTape2-100-markers.sal
	$(SALEXTRACT) -o . TestTape2-100-markers.sal
	$(READTAPE) -v -tap -ascii -linefeed -scale=5.0 -outp=results/ TestTape2-100-markers/meta.json
	for expected in expected_results/*.tap; do \
		[ -r "$${expected}" ] || continue; \
		created="results/$$(basename "$${expected}")"; \
		cmp "$${expected}" "$${created}" || { echo "$${expected} does not match $${created}." >&2; exit 1; }; \
	done
TestTape2-100-markers.sal:
	curl -O https://archive.decromancer.ca/bits/9Track/TestTape2-100-markers.sal
clean:
	rm -rf TestTape2-100-markers/

.PHONY: all test clean
