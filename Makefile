# Makefile
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Werror -fPIC -O2 -g -std=gnu99
LDFLAGS ?= -shared
INC     := -I.

SRC  := lwp.c sched_rr.c
OBJS := $(SRC:.c=.o) magic64.o

.PHONY: all clean test

all: liblwp.so

liblwp.so: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

lwp.o: lwp.c lwp.h fp.h
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

sched_rr.o: sched_rr.c lwp.h
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

magic64.o: magic64.S lwp.h fp.h
	$(CC) -c $< -o $@

clean:
	rm -f *.o liblwp.so tests/*.out core.* tmpfile.* t_script.*

test: all
	$(MAKE) -C tests

