CXX ?= g++
CXXFLAGS = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections

all: bot http

bot: bot.cpp
	$(CXX) $(CXXFLAGS) -o bot bot.cpp

http: http.cpp
	$(CXX) $(CXXFLAGS) -o http http.cpp

clean:
	rm -f bot http
