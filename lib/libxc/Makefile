#
# Make Xinu's C run-time support library
#

# Name of this library
LIBNAME := libxc

# C files to compile (.c)
CFILES  := abs.c      \
           atoi.c     \
           atol.c     \
           bzero.c    \
           ctype_.c   \
           doprnt.c   \
           doscan.c   \
           fgetc.c    \
           fgets.c    \
           fprintf.c  \
           fputc.c    \
           fputs.c    \
           fscanf.c   \
           labs.c     \
           memchr.c   \
           memcmp.c   \
           memcpy.c   \
           memmove.c  \
           memset.c   \
           printf.c   \
           qsort.c    \
           rand.c     \
           sprintf.c  \
           sscanf.c   \
           strchr.c   \
           strcat.c   \
           strcmp.c   \
           strcpy.c   \
           strlcpy.c  \
           strlen.c   \
           strncat.c  \
           strncmp.c  \
           strncpy.c  \
           strnlen.c  \
           strrchr.c  \
           strstr.c

# Add deprecated malloc and free if needed
CFILES  += malloc.c free.c

# calloc/realloc over the same L2MEM heap (used by the shell, by users of
# libxc, and by the Java virtual machine once it is built in).
CFILES  += calloc.c realloc.c

# Compiler run-time helpers (64-bit division, bit scanning); no libgcc.
CFILES  += compiler_rt.c

# Assembly files to compile (.S)
SFILES  :=

# Directory in which to place the output library, and
# location of common Makerules
LIBDIR  := ..

include $(LIBDIR)/Makerules
