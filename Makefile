# Makefile for RDT 3.0 Project
# Course:  COP4635 Sys & Net II  –  Spring 2026
# Authors: Chris Jackson, Ben Green
#
# Builds three executables:
#   sender   – RDT 3.0 sender
#   receiver – RDT 3.0 receiver
#   network  – RDT network simulator

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -pthread

# ── Sources ───────────────────────────────────────────────────────────────────
SENDER_SRCS   := senderMain.cpp   RDTSender.cpp
RECEIVER_SRCS := receiverMain.cpp RDTReceiver.cpp
NETWORK_SRCS  := networkMain.cpp  RDTNetwork.cpp

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all clean

all: sender receiver network

sender: $(SENDER_SRCS) RDTSender.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SENDER_SRCS)

receiver: $(RECEIVER_SRCS) RDTReceiver.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(RECEIVER_SRCS)

network: $(NETWORK_SRCS) RDTNetwork.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(NETWORK_SRCS)

clean:
	rm -f sender receiver network *.o