# SPDX-License-Identifier: GPL-3.0-or-later
# Host test bench for the 2.4G/BT wake detectors and the SSH client: plain
# C++17, no ESP-IDF.
#   make test            builds everything and runs all tests (non-zero exit on failure)
#   make rfreplay        the replay tool only (build/rfreplay)
#   make CXX=clang++     another compiler (default g++)
# Two builds of the same sources: "default" (as the firmware) and "esb2m"
# (with the experimental -DRF_PROTO_ESB2M protocol).
CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -I../src
SRC       = ../src
CORE      = $(SRC)/rf_dsp.cpp $(SRC)/rf_detect.cpp $(SRC)/rf_proto.cpp \
            $(SRC)/proto_esb1m.cpp $(SRC)/proto_btpage.cpp $(SRC)/proto_esb2m.cpp
HDRS      = $(wildcard $(SRC)/*.h) $(wildcard *.h)
B         = build

.PHONY: all test clean rfreplay ssh
ssh: $(B)/ssh_host
	sh test_ssh.sh $(B)

all: test

$(B):
	mkdir -p $(B)

# name: sources, defines
$(B)/test_detect: test_detect.cpp legacy_ref.cpp $(CORE) $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -o $@ test_detect.cpp legacy_ref.cpp $(CORE)
$(B)/test_detect_esb2m: test_detect.cpp legacy_ref.cpp $(CORE) $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -DRF_PROTO_ESB2M -o $@ test_detect.cpp legacy_ref.cpp $(CORE)
$(B)/test_false: test_false.cpp $(CORE) $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -DRF_PROTO_ESB2M -o $@ test_false.cpp $(CORE)
$(B)/btc_selftest: btc_selftest.cpp $(SRC)/btc.h | $(B)
	$(CXX) $(CXXFLAGS) -o $@ btc_selftest.cpp
$(B)/rfreplay: rfreplay.cpp sigmf.h $(CORE) $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -DRF_PROTO_ESB2M -o $@ rfreplay.cpp $(CORE)
$(B)/mksigmf: mksigmf.cpp sigmf.h synth.h $(CORE) $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -DRF_PROTO_ESB2M -o $@ mksigmf.cpp $(CORE)

# The SSH client (src/ssh_client.cpp) with Monocypher, for test_ssh.sh
MONO      = ../components/monocypher
$(B)/monocypher.o: $(MONO)/monocypher.c $(MONO)/monocypher.h | $(B)
	$(CC) -O2 -c -o $@ $<
$(B)/monocypher-ed25519.o: $(MONO)/monocypher-ed25519.c $(MONO)/monocypher-ed25519.h | $(B)
	$(CC) -O2 -c -o $@ $<
$(B)/ssh_host: ssh_host.cpp $(SRC)/ssh_client.cpp $(SRC)/ssh_client.h $(B)/monocypher.o $(B)/monocypher-ed25519.o | $(B)
	$(CXX) $(CXXFLAGS) -I$(MONO) -o $@ ssh_host.cpp $(SRC)/ssh_client.cpp $(B)/monocypher.o $(B)/monocypher-ed25519.o

rfreplay: $(B)/rfreplay

test: $(B)/btc_selftest $(B)/test_detect $(B)/test_detect_esb2m $(B)/test_false $(B)/rfreplay $(B)/mksigmf $(B)/ssh_host
	@echo "== btc self-test"; $(B)/btc_selftest
	@echo "== detectors (default build)"; $(B)/test_detect
	@echo "== detectors (with the experimental esb2m)"; $(B)/test_detect_esb2m
	@echo "== false detections"; $(B)/test_false
	@echo "== rfreplay end to end"; sh test_rfreplay.sh $(B)
	@echo "== SSH client against OpenSSH"; sh test_ssh.sh $(B)
	@echo "== PC scripts (update, setup)"; sh test_pc_scripts.sh
	@echo "ALL HOST TESTS PASSED"

clean:
	rm -rf $(B)
