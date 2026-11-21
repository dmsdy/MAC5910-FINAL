CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O2 -pthread

all: tracker peer

tracker: tracker.cpp
	$(CXX) $(CXXFLAGS) -o tracker tracker.cpp

# o SHA-256 vem do OpenSSL (libcrypto)
peer: peer.cpp conn.h
	$(CXX) $(CXXFLAGS) -o peer peer.cpp -lcrypto

clean:
	rm -f tracker peer

.PHONY: all clean
