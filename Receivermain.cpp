/*
 * File:    receiverMain.cpp
 * Purpose: Driver program for the RDT 3.0 receiver. Calls
 *          RDTReceiver::receiveMessage() and prints the assembled message.
 *
 * Usage:   ./receiver <port>
 *
 * Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#include "RDTReceiver.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }

    int port = std::stoi(argv[1]);

    RDTReceiver receiver(port);

    std::string msg = receiver.receiveMessage();
    if (msg.empty()) {
        std::cerr << "[Receiver] No message received or error occurred.\n";
        return 1;
    }

    std::cout << "\n[Receiver] Assembled message: \"" << msg << "\"\n";
    return 0;
}