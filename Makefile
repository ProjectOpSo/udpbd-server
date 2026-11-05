BIN = udpbd-server
OBJS = main.o

CXX ?= g++
CXXFLAGS ?= -O2

$(BIN): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

all: $(BIN)

clean:
	rm -f $(BIN) $(OBJS)

install: $(BIN)
	cp $(BIN) $(PS2DEV)/bin

.PHONY: all clean install
