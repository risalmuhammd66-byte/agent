CXX     ?= g++
ZIG     ?= /tmp/zig-linux-x86_64-0.13.0/zig

# Native dynamic build (requires OpenSSL dev headers + libs)
CXXFLAGS_NATIVE = -O3 -s -flto -pthread -Wall -Wextra -fomit-frame-pointer -fno-exceptions -fno-rtti
LIBS_NATIVE = -lssl -lcrypto

# Fully-static musl build (TLS enabled, runs on ANY Linux device - no glibc deps)
ZIG_CXXFLAGS = -static -O2 -s -fno-exceptions -fno-rtti -pthread
X86_LIBATOMIC = /tmp/ossl-static/x86/libatomic.a

.PHONY: all static native clean

all: static

# -----------------------------------------------------------------------
# Fully-static multi-arch binaries (musl + OpenSSL TLS). These run on any
# Linux device: Ubuntu/Debian/Alpine/OpenWrt/Termux, old & new kernels.
# -----------------------------------------------------------------------
static:
	@echo "[*] Building statically-linked flood binaries..."
	$(ZIG) c++ -target x86_64-linux-musl -mcpu=x86_64 $(ZIG_CXXFLAGS) -I/tmp/ossl-static/x86_64/include methods.cpp -o flood-x86_64 /tmp/ossl-static/x86_64/lib/libssl.a /tmp/ossl-static/x86_64/lib/libcrypto.a && echo "[+] flood-x86_64 OK"
	$(ZIG) c++ -target aarch64-linux-musl $(ZIG_CXXFLAGS) -I/tmp/ossl-static/aarch64/include methods.cpp -o flood-aarch64 /tmp/ossl-static/aarch64/lib/libssl.a /tmp/ossl-static/aarch64/lib/libcrypto.a && echo "[+] flood-aarch64 OK"
	$(ZIG) c++ -target arm-linux-musleabihf $(ZIG_CXXFLAGS) -I/tmp/ossl-static/armv7l/include methods.cpp -o flood-armv7l /tmp/ossl-static/armv7l/lib/libssl.a /tmp/ossl-static/armv7l/lib/libcrypto.a && echo "[+] flood-armv7l OK"
	$(ZIG) c++ -target x86-linux-musl -march=i686 $(ZIG_CXXFLAGS) -I/tmp/ossl-static/x86/include methods.cpp -o flood-x86 /tmp/ossl-static/x86/lib/libssl.a /tmp/ossl-static/x86/lib/libcrypto.a $(X86_LIBATOMIC) && echo "[+] flood-x86 OK"

# Native (dynamic) build using system OpenSSL
native: methods.cpp
	$(CXX) $(CXXFLAGS_NATIVE) methods.cpp -o flood $(LIBS_NATIVE)
	strip -s flood 2>/dev/null || true

clean:
	rm -f flood flood-*