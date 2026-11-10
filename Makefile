CXX      := g++
CXXFLAGS := -std=c++2c -Wall -Wextra -I. -Ilibs -Itest -Isrc

TEST_MAIN := test/test_main.cpp
TEST_SRCS := $(filter-out $(TEST_MAIN), $(wildcard test/*.cpp))
TEST_HDRS := $(wildcard src/*/*.hpp)

.PHONY: all test core clean

all: test core

build/tests: $(TEST_MAIN) $(TEST_SRCS) $(TEST_HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(TEST_MAIN) $(TEST_SRCS) -o $@

test: build/tests
	./build/tests

build/csvmain: src/cli/CSVmain.cpp $(TEST_HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) src/cli/CSVmain.cpp -o $@

core: build/csvmain

clean:
	rm -rf build
