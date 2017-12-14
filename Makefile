TARGET = dstcnv
LIBDIR = libdstdec
OBJECT = $(TARGET).o
LIB    = $(LIBDIR)/libdstdec.a
CFLAGS = -O2 -Wall -I$(LIBDIR)
CC     = gcc
RM     = rm -f
ifeq ($(OS),Windows_NT)
	CFLAGS = -O2 -Wall -D_FILE_OFFSET_BITS=64 -I$(LIBDIR)
	RM = cmd.exe /C del
endif

$(TARGET) : $(OBJECT) $(LIB)
	$(CC) $(CFLAGS) $(OBJECT) $(LIB) -lpthread -o $(TARGET)

$(LIB) :
	$(MAKE) -C $(LIBDIR)

.PHONY : all clean

all : $(LIB) $(TARGET)

clean :
	$(RM) $(TARGET) $(OBJECT)
	$(MAKE) -C $(LIBDIR) clean
