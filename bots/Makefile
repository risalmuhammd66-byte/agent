CXX      ?= g++
CXXFLAGS  = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread

METHODS = dns udp ldap ssdp home udpbypass tcp socket ovh tcpmix tcpbypass ack \
          game rainbow rocket roblox fivem pubg fortnite warthunder counter samp \
          subnet icmp httpx rapidflood tls tlsx bypass browser cache cloudflare

all: bot http https flood symlinks

bot: bot.cpp
	$(CXX) $(CXXFLAGS) -static -o bot bot.cpp

http: http.cpp
	$(CXX) $(CXXFLAGS) -static -o http http.cpp

https: https.cpp
	$(CXX) $(CXXFLAGS) -o https https.cpp -lssl -lcrypto

flood: flood.cpp
	$(CXX) $(CXXFLAGS) -o flood flood.cpp -lssl -lcrypto

symlinks: flood
	@for m in $(METHODS); do \
		ln -sf flood $$m; \
	done

clean:
	rm -f bot http https flood $(METHODS)
