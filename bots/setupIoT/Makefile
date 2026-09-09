# Makefile for Android & IoT Bot Compilation
CXX ?= g++
CXXFLAGS = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -static-libstdc++ -static-libgcc

all: bot flood

bot: ../bot.cpp
	$(CXX) $(CXXFLAGS) -static -o bot ../bot.cpp

flood: ../flood.cpp
	$(CXX) $(CXXFLAGS) -DNO_SSL -static -o flood ../flood.cpp

# Lightweight IoT build without OpenSSL dependency
iot-nossl: ../bot.cpp ../flood.cpp
	$(CXX) $(CXXFLAGS) -static -o bot ../bot.cpp
	$(CXX) $(CXXFLAGS) -DNO_SSL -static -o flood ../flood.cpp

clean:
	rm -f bot flood
