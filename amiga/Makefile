CC      = m68k-amigaos-gcc
CFLAGS  = -O2 -Wall -noixemul -m68020
LDFLAGS = -noixemul

TARGET  = AmiRenderSwarm
SRCS    = main.c net.c proto.c worker.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c net.h proto.h worker.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean