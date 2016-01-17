
TARGET=qgis-schedulerd

SOURCE=qgis-schedulerd.c qgis_process.c qgis_process_list.c fcgi_state.c qgis_config.c #log.c
HEADER=qgis_process.h qgis_process_list.h fcgi_state.h qgis_config.h #log.h defaults.h

CFLAGS += -Wall #-Wextra
CFLAGS += -pipe -ggdb -O0
#CFLAGS += -pipe -O2
#CFLAGS += -pipe -O3 -march=native
#CFLAGS += -DNDEBUG

CFLAGS += -pthread
LDFLAGS += -pthread
LDLIBS += -liniparser #-lfcgi

# ---------------------------------

OBJECT=$(patsubst %.c,%.o,$(SOURCE))

all: $(TARGET)

$(TARGET): $(OBJECT)
#	$(CC) $(LDFLAGS) -o $@ $(LOADLIBES) $(LDLIBS) $^
$(OBJECT): Makefile $(HEADER)


clean:
	-rm $(OBJECT) $(TARGET)


.PHONY: all clean
