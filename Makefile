CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pedantic
SANFLAGS := -O1 -g -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer

HEADERS := $(wildcard src/*.h)

all: build/bptree_bench build/gen_dataset build/test_bptree

build/bptree_bench: src/main.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp

build/gen_dataset: tools/gen_dataset.cpp | build
	$(CXX) $(CXXFLAGS) -o $@ tools/gen_dataset.cpp

build/test_bptree: tests/test_bptree.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_bptree.cpp

build/test_bptree_san: tests/test_bptree.cpp $(HEADERS) | build
	$(CXX) $(SANFLAGS) -o $@ tests/test_bptree.cpp

build:
	mkdir -p build

test: build/test_bptree
	./build/test_bptree build

# same tests under AddressSanitizer + UndefinedBehaviorSanitizer
test-san: build/test_bptree_san
	./build/test_bptree_san build

clean:
	rm -rf build

.PHONY: all test test-san clean
