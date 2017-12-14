TARGET = libdstdec.a
CFLAGS = -O2 -Wall -msse2
OBJECT = $(patsubst %.c,%.o,$(wildcard *.c))
CC     = gcc
AR     = ar
RANLIB = ranlib
RM     = rm -f
ifeq ($(OS),Windows_NT)
	RM = cmd.exe /C del
endif

$(TARGET) : $(OBJECT)
	$(AR) rv $@ $?
	$(RANLIB) $@

.PHONY : all clean

all : $(TARGET) $(OBJECT)

clean :
	$(RM) $(TARGET) $(OBJECT)
