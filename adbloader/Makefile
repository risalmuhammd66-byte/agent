CXX      ?= g++
CXXFLAGS  = -O2 -pthread -std=c++17 -Wall

all: adbloader

adbloader: main.cpp
	$(CXX) $(CXXFLAGS) -o adbloader main.cpp

clean:
	rm -f adbloader
