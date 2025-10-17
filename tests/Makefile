# tests/Makefile
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g
INC = -I..

TESTS = 00_symbols 01_create_ids 02_rr_order 03_bootable_ctx 04_start_yield 05_wait_status 06_set_scheduler 99_stress test_min 07_wait_blocking 08_migrate_preserve_order 09_status_low8 11_tid_lookup 12_fpu_context

.PHONY: all clean test
all: $(TESTS:=.out)

%.out: %.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< -L.. -llwp -lm -Wl,-rpath=..

clean:
	rm -f *.out

test: all
	@echo "Running stress…"
	@LD_LIBRARY_PATH=.. ./99_stress.out

