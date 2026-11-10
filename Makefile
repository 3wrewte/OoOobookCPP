CXX      := g++
CXXFLAGS := -std=c++2c -Wall -Wextra -I. -Ilibs -Itest -Isrc

TEST_MAIN := test/test_main.cpp
TEST_SRCS := $(filter-out $(TEST_MAIN), $(wildcard test/*.cpp))
TEST_HDRS := $(wildcard src/*/*.hpp)

.PHONY: all test clean

all: test

build/tests: $(TEST_MAIN) $(TEST_SRCS) $(TEST_HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(TEST_MAIN) $(TEST_SRCS) -o $@

test: build/tests
	./build/tests

clean:
	rm -rf build
