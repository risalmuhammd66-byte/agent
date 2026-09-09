CXX      ?= g++
CXXFLAGS  = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread

all: bot http https flood

bot: bot.cpp
	$(CXX) $(CXXFLAGS) -static -o bot bot.cpp

http: http.cpp
	$(CXX) $(CXXFLAGS) -static -o http http.cpp

https: https.cpp
	$(CXX) $(CXXFLAGS) -o https https.cpp -lssl -lcrypto

flood: flood.cpp
	$(CXX) $(CXXFLAGS) -static -o flood flood.cpp

clean:
	rm -f bot http https flood

