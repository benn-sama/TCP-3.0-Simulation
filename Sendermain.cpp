/*
 * File:    senderMain.cpp
 * Purpose: Driver program for the RDT 3.0 sender. Prompts the user for a
 *          message and calls RDTSender::sendMessage() to transmit it via
 *          the network simulator to the receiver.
 *
 * Usage:   ./sender <localPort> <networkHost> <networkPort> <receiverHost> <receiverPort>
 *
 * Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#include "RDTSender.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0]
                  << " <localPort> <networkHost> <networkPort>"
                  << " <receiverHost> <receiverPort>\n";
        return 1;
    }

    int         localPort  = std::stoi(argv[1]);
    std::string netwHost   = argv[2];
    int         netwPort   = std::stoi(argv[3]);
    std::string destHost   = argv[4];
    int         destPort   = std::stoi(argv[5]);

    RDTSender sender(localPort, netwHost, netwPort, destHost, destPort);

    std::string message;
    std::cout << "Enter message to send: ";
    std::getline(std::cin, message);

    return sender.sendMessage(message);
}