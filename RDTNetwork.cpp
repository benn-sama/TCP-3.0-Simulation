/*
 * File:    RDTNetwork.cpp
 * Purpose: Implementation of the RDT network simulator.
 *          - Receives 54-byte UDP packets from either the sender or receiver, then it drops, delays, or
 *          corrupts them before forwarding to the correct destination using the network header embedded in each packet. 
 *           - Once in a while prints traffic statistics

 * Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#include "RDTNetwork.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

// full packet is 54 bytes (network header 44 bytes + transport segment 10 bytes)
static const int PACKET_SIZE = 54;

// network-header field offsets
static const int OFF_DEST_IP   = 22;  // 16 bytes
static const int OFF_DEST_PORT = 38;  //  6 bytes

// transport-segment offset and corruption flag byte
static const int OFF_SEG         = 44;
static const int SEG_CORRUPT_OFF = 2;  // byte [46] = corrupt flag

// delay: 1.5× the sender's 2-second timeout = 3 seconds.
// This ensures the sender's timer fires before the delayed packet arrives
static const int DELAY_SECONDS = 3;

// thread arg delay and forward
struct DelayArg {
    int  socketFd;
    char packet[PACKET_SIZE];
    int  packetLen;
};


RDTNetwork::RDTNetwork(int localPort,
                       int lostPercent,
                       int delayedPercent,
                       int errorPercent)
    : socketFd(-1),
      localPort(localPort),
      lostPercent(lostPercent),
      delayedPercent(delayedPercent),
      errorPercent(errorPercent),
      totalReceived(0),
      totalForwarded(0),
      totalDropped(0),
      totalDelayed(0),
      totalCorrupted(0)
{
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // creates UDP socket
    socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        std::cerr << "[Network] socket() failed: " << strerror(errno) << "\n";
        return;
    }

    // binds to local port
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(localPort));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socketFd,
             reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        std::cerr << "[Network] bind() failed: " << strerror(errno) << "\n";
        close(socketFd);
        socketFd = -1;
        return;
    }

    // prints hostname and bounds port
    struct sockaddr_in boundAddr{};
    socklen_t addrLen = sizeof(boundAddr);
    getsockname(socketFd,
                reinterpret_cast<struct sockaddr*>(&boundAddr),
                &addrLen);

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));

    std::cout << "[Network] Host: " << hostname
              << "  Port: " << ntohs(boundAddr.sin_port) << "\n";
    std::cout << "[Network] Loss=" << lostPercent
              << "%  Delay=" << delayedPercent
              << "%  Error=" << errorPercent << "%\n";
}

// destructor
RDTNetwork::~RDTNetwork() {
    if (socketFd >= 0) {
        close(socketFd);
        socketFd = -1;
    }
}

// forwardPacket
void RDTNetwork::forwardPacket(const char* packet, int packetLen) {
    // get destination IP (bytes 22–37) and port (bytes 38–43) from the network header as null-terminated strings.
    char destIPbuf[17]   = {};
    char destPortbuf[7]  = {};

    std::memcpy(destIPbuf,   packet + OFF_DEST_IP,   16);
    std::memcpy(destPortbuf, packet + OFF_DEST_PORT,  6);

    int destPort = std::atoi(destPortbuf);

    struct sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port   = htons(static_cast<uint16_t>(destPort));

    if (inet_pton(AF_INET, destIPbuf, &destAddr.sin_addr) <= 0) {
        std::cerr << "[Network] Invalid destination IP: " << destIPbuf << "\n";
        return;
    }

    ssize_t sent = sendto(socketFd,
                          packet, packetLen, 0,
                          reinterpret_cast<struct sockaddr*>(&destAddr),
                          sizeof(destAddr));
    if (sent < 0)
        std::cerr << "[Network] sendto() failed: " << strerror(errno) << "\n";
    else {
        ++totalForwarded;
        std::cout << "[Network] Forwarded to " << destIPbuf
                  << ":" << destPort << "\n";
    }
}

// corrupt packets
void RDTNetwork::corruptPacket(char* packet, int /*packetLen*/) {
    // Flip the corrupt flag (byte 46) in the transport segment
    packet[OFF_SEG + SEG_CORRUPT_OFF] =
        (packet[OFF_SEG + SEG_CORRUPT_OFF] == 0) ? 1 : 0;
    ++totalCorrupted;
    std::cout << "[Network] Packet corrupted.\n";
}

// thread entry for delay and forward
static void* delayThread(void* arg) {
    DelayArg* da = static_cast<DelayArg*>(arg);

    sleep(DELAY_SECONDS);  // delay must be > sender's retransmit timeout

    // resolve destination and forward
    char destIPbuf[17]  = {};
    char destPortbuf[7] = {};
    std::memcpy(destIPbuf,   da->packet + OFF_DEST_IP,   16);
    std::memcpy(destPortbuf, da->packet + OFF_DEST_PORT,  6);

    int destPort = std::atoi(destPortbuf);

    struct sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port   = htons(static_cast<uint16_t>(destPort));
    inet_pton(AF_INET, destIPbuf, &destAddr.sin_addr);

    sendto(da->socketFd,
           da->packet, da->packetLen, 0,
           reinterpret_cast<struct sockaddr*>(&destAddr),
           sizeof(destAddr));

    std::cout << "[Network] Delayed packet forwarded to "
              << destIPbuf << ":" << destPort << "\n";

    delete da;
    pthread_exit(nullptr);
}

// delayAndForward
void RDTNetwork::delayAndForward(const char* packet, int packetLen) {
    DelayArg* arg = new DelayArg();
    arg->socketFd  = socketFd;
    arg->packetLen = packetLen;
    std::memcpy(arg->packet, packet, packetLen);

    pthread_t tid;
    if (pthread_create(&tid, nullptr, delayThread, arg) != 0) {
        std::cerr << "[Network] pthread_create() failed: " << strerror(errno) << "\n";
        delete arg;
        return;
    }
    pthread_detach(tid);

    ++totalDelayed;
    std::cout << "[Network] Packet queued for delayed forwarding ("
              << DELAY_SECONDS << "s).\n";
}

// printStats
void RDTNetwork::printStats() const {
    std::cout << "\n──────────── Network Traffic Statistics ────────────\n"
              << "  Total received  : " << totalReceived  << "\n"
              << "  Total forwarded : " << totalForwarded << "\n"
              << "  Total dropped   : " << totalDropped   << "\n"
              << "  Total delayed   : " << totalDelayed   << "\n"
              << "  Total corrupted : " << totalCorrupted << "\n"
              << "────────────────────────────────────────────────────\n\n";
}

/*
 * Main forwarding loop:
 *   1. recvfrom() a 54-byte packet.
 *   2. Increment totalReceived.
 *   3. RNG for lost (drop), delayed, or corrupt.
 *      Priority: lost > delayed > corrupt (a packet cannot be both dropped
 *      and delayed, but it can be delayed AND corrupted).
 *   4. Print statistics every other packet received.
 */
void RDTNetwork::run() {
    if (socketFd < 0) {
        std::cerr << "[Network] Socket is not open – cannot run.\n";
        return;
    }

    std::cout << "[Network] Forwarding loop started.\n";

    while (true) {
        char buf[PACKET_SIZE] = {};
        struct sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);

        ssize_t n = recvfrom(socketFd,
                             buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&fromAddr),
                             &fromLen);
        if (n < 0) {
            std::cerr << "[Network] recvfrom() failed: " << strerror(errno) << "\n";
            continue;
        }

        ++totalReceived;
        std::cout << "[Network] Received packet #" << totalReceived << "\n";

        int roll = std::rand() % 100;  // 0–99

        // imulates packet loss
        if (roll < lostPercent) {
            ++totalDropped;
            std::cout << "[Network] Packet DROPPED (loss simulation).\n";
            if (totalReceived % 2 == 0) printStats();
            continue;  // do not forward
        }

        // simulates corruption (may combine with delay below) 
        int errRoll = std::rand() % 100;
        if (errRoll < errorPercent) {
            corruptPacket(buf, static_cast<int>(n));
        }

        // simulates delay or immediate forward
        int delayRoll = std::rand() % 100;
        if (delayRoll < delayedPercent) {
            delayAndForward(buf, static_cast<int>(n));
        } else {
            forwardPacket(buf, static_cast<int>(n));
        }

        // prints stats every other packet 
        if (totalReceived % 2 == 0) printStats();
    }
}