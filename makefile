PORT=55019

CFLAGS=-DPORT=$(PORT) -g -Wall

GCC=gcc

TARGET=battle

SOURCE=battle.c

ITEMS=$(SOURCE:.c=.o)

all: $(TARGET)

$(TARGET): $(ITEMS)
	$(GCC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(GCC) $(CFLAGS) -c $<

clean:
	rm -f $(TARGET) $(ITEMS)

port:
	make PORT=$(PORT)
