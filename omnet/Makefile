CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CAN_SRC := src/can/CanCodec.cc src/can/CanController.cc src/can/CanBus.cc src/can/CanReceiver.cc

.PHONY: test clean
test: build/test_can_core
	./build/test_can_core

build/test_can_core: tests/test_can_core.cc tests/controller_rule_fixture.csv $(CAN_SRC) src/can/CanFrame.h src/can/CanCodec.h src/can/CanController.h src/can/CanBus.h src/can/PassiveWireDecoder.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc/can tests/test_can_core.cc $(CAN_SRC) -o $@

clean:
	rm -rf build
