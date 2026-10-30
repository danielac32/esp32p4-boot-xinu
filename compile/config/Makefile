#
# Make the Xinu configuration program
#

CC      = gcc
LEX     = flex
YACC    = bison -y    # Flag enables yacc-compatible filenames
YFLAGS  = -d
# The configuration program is old (K&R-era) C; modern GCC rejects its
# implicit function declarations.  Keep them as warnings.
CFLAGS  = -std=gnu89 -Wno-implicit-function-declaration -Wno-implicit-int \
          -Wno-int-conversion -Wno-builtin-declaration-mismatch
LDFLAGS =

CONFIG  = config
OBJECTS = scan.o parse.o config.o

$(CONFIG): $(OBJECTS)

scan.o: scan.l parse.c

parse.o: parse.y

y.tab.h: parse.o

config.o: config.c y.tab.h

clean:
	rm -f $(CONFIG) *.o y.tab.h parse.c
