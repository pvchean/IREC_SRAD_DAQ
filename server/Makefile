CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic

all: telemetry_server test_client_bin frame_compat_bin

telemetry_server: telemetry_server.cpp telem_proto.h tp_util.hpp
	$(CXX) $(CXXFLAGS) -static-libstdc++ -static-libgcc -o $@ telemetry_server.cpp

test_client_bin: test/test_client.cpp telem_proto.h tp_util.hpp
	$(CXX) $(CXXFLAGS) -o $@ test/test_client.cpp

frame_compat_bin: test/frame_compat.cpp telem_proto.h tp_util.hpp telem_frame.h
	$(CXX) $(CXXFLAGS) -o $@ test/frame_compat.cpp

test: all
	./frame_compat_bin
	sh test/run_tests.sh

clean:
	rm -f telemetry_server test_client_bin frame_compat_bin

.PHONY: all test clean
