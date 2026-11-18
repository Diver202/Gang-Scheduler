CXX ?= g++
CXXFLAGS ?= -O3 -mcpu=native -std=c++20 -Wall -Wextra -pthread
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS = $(shell pkg-config --libs sdl2) -lbpf

SRC = src/main.cpp src/cloth.cpp
HDR = src/vec.h src/cloth.h src/barrier.h src/viewer.h src/gang.h ../scheduler/gang_common.h

vbd: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -I../scheduler $(SDL_CFLAGS) $(SRC) -o $@ $(SDL_LIBS)

clean:
	rm -f vbd

.PHONY: clean
