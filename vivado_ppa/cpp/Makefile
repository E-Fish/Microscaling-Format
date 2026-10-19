# Builds the C++ version of vivado_ppa.
#
#   make          build ../vivado_ppa (next to designs/ and settings.ini)
#   make clean    remove the build files and the program
#
# Needs a C++17 compiler (g++ 9 or newer). With g++ 8, run:  make LDLIBS=-lstdc++fs

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDFLAGS  ?= -pthread
LDLIBS   ?=

SRC    := $(wildcard src/*.cpp)
OBJ    := $(SRC:src/%.cpp=build/%.o)
HDR    := $(wildcard src/*.hpp)
TARGET := ../vivado_ppa

all: $(TARGET)

# Link: combine the compiled pieces into one program.
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Compile: each .cpp becomes a .o (recompiled if it or any header changes).
build/%.o: src/%.cpp $(HDR) | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

.PHONY: all clean
