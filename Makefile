
TARGET=qgis-scheduler

SOURCE=main.c
HEADER=

CFLAGS += -Wall
CFLAGS += -pipe -ggdb -O0
#CFLAGS += -pipe -O2
#CFLAGS += -pipe -O3 -march=native
#CFLAGS += -DNDEBUG


# ---------------------------------

OBJECT=$(patsubst %.c,%.o,$(SOURCE))

all: $(TARGET)

$(TARGET): $(OBJECT)
$(OBJECT): Makefile $(HEADER)


clean:
	-rm $(OBJECT) $(TARGET)


.PHONY: all clean
