# Host-side unit tests. Usage: make -C test
CXX ?= c++
CXXFLAGS ?= -std=c++11 -O1 -Wall -Wextra -Werror
INCLUDES = -I..

all: run

test_timecode: test_timecode.cpp ../timecode.cpp ../timecode.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ test_timecode.cpp ../timecode.cpp

test_tzdata: test_tzdata.cpp ../timecode.cpp ../tzdata.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ test_tzdata.cpp ../timecode.cpp

run: test_timecode test_tzdata
	./test_timecode
	./test_tzdata

clean:
	rm -f test_timecode test_tzdata

.PHONY: all run clean
