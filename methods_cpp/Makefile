CXX ?= g++
CXXFLAGS = -O3 -s -flto -pthread -Wall -Wextra -fomit-frame-pointer
LIBS = -lssl -lcrypto
TARGET = flood

all: $(TARGET)

$(TARGET): methods.cpp
	$(CXX) $(CXXFLAGS) methods.cpp -o $(TARGET) $(LIBS)
	strip -s $(TARGET) 2>/dev/null || true

clean:
	rm -f $(TARGET)
