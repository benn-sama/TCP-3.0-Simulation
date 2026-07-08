/*
 * File:    networkMain.cpp
 * Purpose: Driver program for the RDT network simulator. Instantiates an
 *          RDTNetwork with the given port and simulation percentages, then
 *          starts the forwarding loop.
 *
 * Usage:   ./network <port> <lostPercent> <delayedPercent> <errorPercent>
* Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#include "RDTNetwork.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <port> <lostPercent> <delayedPercent> <errorPercent>\n";
        return 1;
    }

    int port           = std::stoi(argv[1]);
    int lostPercent    = std::stoi(argv[2]);
    int delayedPercent = std::stoi(argv[3]);
    int errorPercent   = std::stoi(argv[4]);

    RDTNetwork network(port, lostPercent, delayedPercent, errorPercent);
    network.run();

    return 0;
}