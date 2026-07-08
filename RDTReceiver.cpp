/*
 * File:    RDTReceiver.cpp
 * Purpose: Implementation of the RDT 3.0 (Alternating Bit Protocol) receiver.
 *          Listens on a UDP socket for 54-byte packets from the network
 *          simulator. Validates sequence numbers and corruption flags, sends
 *          ACKs back to the network (which forwards them to the sender), and
 *          reassembles payload bytes into the complete original message.
 * Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#include "RDTReceiver.hpp"

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ─── Packet layout constants ─────────────────────────────────────────────────
// Mirrors the layout used by RDTSender (see RDTSender.cpp for full diagram).
// Total UDP payload = 54 bytes
//   Network header  : bytes  0–43
//   Transport segment: bytes 44–53
//     [44] seq_num   : 0 or 1
//     [45] ack_num   : 0 or 1
//     [46] corrupt   : 0 = clean, 1 = corrupt
//     [47] is_last   : 1 if this is the final segment
//     [48] data_len  : number of valid payload bytes
//     [49–53]        : payload (up to 5 bytes)

static const int PACKET_SIZE   = 54;
static const int PAYLOAD_BYTES = 5;

// Network-header field offsets
static const int OFF_SRC_IP    =  0;  // 16 bytes
static const int OFF_SRC_PORT  = 16;  //  6 bytes
static const int OFF_DEST_IP   = 22;  // 16 bytes
static const int OFF_DEST_PORT = 38;  //  6 bytes
static const int OFF_SEG       = 44;  // start of transport segment

// Transport-segment offsets (relative to OFF_SEG)
static const int SEG_SEQ     = 0;
static const int SEG_ACK     = 1;
static const int SEG_CORRUPT = 2;
static const int SEG_LAST    = 3;
static const int SEG_LEN     = 4;
static const int SEG_DATA    = 5;

// ─── Constructor ─────────────────────────────────────────────────────────────
RDTReceiver::RDTReceiver(int port)
    : socketFd(-1), port(port)
{
    // Create UDP socket
    socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        std::cerr << "[Receiver] socket() failed: " << strerror(errno) << "\n";
        return;
    }

    // Bind to the specified port
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socketFd,
             reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        std::cerr << "[Receiver] bind() failed: " << strerror(errno) << "\n";
        close(socketFd);
        socketFd = -1;
        return;
    }

    // Print hostname and bound port (spec requirement)
    struct sockaddr_in boundAddr{};
    socklen_t addrLen = sizeof(boundAddr);
    getsockname(socketFd,
                reinterpret_cast<struct sockaddr*>(&boundAddr),
                &addrLen);

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));

    std::cout << "[Receiver] Host: " << hostname
              << "  Port: " << ntohs(boundAddr.sin_port) << "\n";
}

// ─── Destructor ───────────────────────────────────────────────────────────────
RDTReceiver::~RDTReceiver() {
    if (socketFd >= 0) {
        close(socketFd);
        socketFd = -1;
    }
}

// ─── receiveMessage ───────────────────────────────────────────────────────────
/*
 * Implements the rdt 3.0 receiver protocol:
 *
 *   Maintain expectedSeq (starts at 0, alternates 0/1).
 *   Loop:
 *     1. recvfrom() a 54-byte packet from the network.
 *     2. If corrupt flag is set → send NAK (ACK with flipped seq) and repeat.
 *     3. If seq_num != expectedSeq → re-ACK the last accepted seq and repeat
 *        (handles duplicate retransmissions from the sender).
 *     4. Accept the segment: append payload bytes to message buffer.
 *     5. Build and send an ACK packet back to the network:
 *          - Swap src ↔ dest fields in the network header so the network
 *            forwards the ACK to the sender.
 *          - Set seq_num and ack_num to expectedSeq.
 *          - Set corrupt = 0.
 *     6. Flip expectedSeq.
 *     7. If is_last == 1 break and return the assembled message.
 *
 * @return the complete text message; empty string on error
 */
std::string RDTReceiver::receiveMessage() {
    if (socketFd < 0) {
        std::cerr << "[Receiver] Socket is not open.\n";
        return "";
    }

    std::string message;        // accumulates accepted payload bytes
    int expectedSeq = 0;        // alternating bit we expect from the sender
    int lastAckedSeq = -1;      // tracks the last seq we ACK'd (for duplicates)

    while (true) {
        // receive packet from network
        std::vector<char> pkt(PACKET_SIZE, 0);
        struct sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);

        ssize_t n = recvfrom(socketFd,
                             pkt.data(), pkt.size(), 0,
                             reinterpret_cast<struct sockaddr*>(&fromAddr),
                             &fromLen);
        if (n < 0) {
            std::cerr << "[Receiver] recvfrom() failed: " << strerror(errno) << "\n";
            return "";
        }
        if (n < PACKET_SIZE) {
            std::cerr << "[Receiver] Short packet received (" << n << " bytes) – ignoring.\n";
            continue;
        }

        int seqNum     = static_cast<unsigned char>(pkt[OFF_SEG + SEG_SEQ]);
        int corruptFlg = static_cast<unsigned char>(pkt[OFF_SEG + SEG_CORRUPT]);
        int isLast     = static_cast<unsigned char>(pkt[OFF_SEG + SEG_LAST]);
        int dataLen    = static_cast<unsigned char>(pkt[OFF_SEG + SEG_LEN]);

        std::cout << "[Receiver] Packet received  seq=" << seqNum
                  << "  corrupt=" << corruptFlg
                  << "  last=" << isLast
                  << "  dataLen=" << dataLen << "\n";

        // builds ACK helper 
        // ACK packet: copy the received packet, swap src/dest in the network
        // header so the network forwards it back to the sender, then set the
        // transport fields appropriately.
        auto buildAck = [&](int ackSeq) -> std::vector<char> {
            std::vector<char> ack(PACKET_SIZE, 0);

            // swap src ↔ dest fields so the network routes the reply to the sender
            char srcIPbuf[16]   = {};
            char srcPortbuf[6]  = {};
            char dstIPbuf[16]   = {};
            char dstPortbuf[6]  = {};

            std::memcpy(srcIPbuf,   &pkt[OFF_SRC_IP],    16);
            std::memcpy(srcPortbuf, &pkt[OFF_SRC_PORT],   6);
            std::memcpy(dstIPbuf,   &pkt[OFF_DEST_IP],   16);
            std::memcpy(dstPortbuf, &pkt[OFF_DEST_PORT],  6);

            // new src = old dest (receiver side)
            std::memcpy(&ack[OFF_SRC_IP],    dstIPbuf,   16);
            std::memcpy(&ack[OFF_SRC_PORT],  dstPortbuf,  6);
            // new dest = old src (sender side)
            std::memcpy(&ack[OFF_DEST_IP],   srcIPbuf,   16);
            std::memcpy(&ack[OFF_DEST_PORT], srcPortbuf,  6);

            // transport segment
            ack[OFF_SEG + SEG_SEQ]     = static_cast<char>(ackSeq);
            ack[OFF_SEG + SEG_ACK]     = static_cast<char>(ackSeq);
            ack[OFF_SEG + SEG_CORRUPT] = 0;
            ack[OFF_SEG + SEG_LAST]    = 0;  // ACKs carry no payload
            ack[OFF_SEG + SEG_LEN]     = 0;

            return ack;
        };

        // corruption check
        if (corruptFlg != 0) {
            // Send NAK: ACK the last successfully received sequence number.
            // If nothing has been received yet, ACK the opposite of expected
            // so the sender retransmits.
            int nakSeq = (lastAckedSeq >= 0) ? lastAckedSeq : (expectedSeq ^ 1);
            std::cout << "[Receiver] Corrupt packet – sending NAK (ack=" << nakSeq << ").\n";

            std::vector<char> nak = buildAck(nakSeq);
            sendto(socketFd,
                   nak.data(), nak.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&fromAddr),
                   fromLen);
            continue;
        }

        // sequence number check
        if (seqNum != expectedSeq) {
            // Duplicate: re-ACK the sequence we already accepted
            std::cout << "[Receiver] Duplicate segment (seq=" << seqNum
                      << ", expected=" << expectedSeq << ") – re-ACKing "
                      << seqNum << ".\n";

            std::vector<char> dupAck = buildAck(seqNum);
            sendto(socketFd,
                   dupAck.data(), dupAck.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&fromAddr),
                   fromLen);
            continue;
        }

        // accept segment
        int validLen = std::min(dataLen, PAYLOAD_BYTES);
        for (int i = 0; i < validLen; ++i)
            message += pkt[OFF_SEG + SEG_DATA + i];

        std::cout << "[Receiver] Accepted seq=" << seqNum
                  << "  payload=\"" << message.substr(message.size() - validLen)
                  << "\"\n";

        // send ACK
        std::vector<char> ack = buildAck(expectedSeq);
        ssize_t sent = sendto(socketFd,
                              ack.data(), ack.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&fromAddr),
                              fromLen);
        if (sent < 0)
            std::cerr << "[Receiver] sendto() ACK failed: " << strerror(errno) << "\n";
        else
            std::cout << "[Receiver] ACK " << expectedSeq << " sent.\n";

        lastAckedSeq = expectedSeq;
        expectedSeq ^= 1;  // flip alternating bit

        // check for end of message
        // Do NOT break immediately after the last segment. The sender may
        // retransmit if our ACK was lost or delayed — and with nobody left
        // to re-ACK it, the sender loops forever. Stay alive for 2× the
        // sender's retransmit timeout (4 s total) and re-ACK any duplicate
        // retransmissions of the last segment before exiting.
        if (isLast) {
            std::cout << "[Receiver] Last segment received. Message: \""
                      << message << "\"\n";

            while (true) {
                fd_set readFds;
                FD_ZERO(&readFds);
                FD_SET(socketFd, &readFds);

                struct timeval tv;
                tv.tv_sec  = 4;  // 2× sender's 2-second timeout
                tv.tv_usec = 0;

                int ready = select(socketFd + 1, &readFds, nullptr, nullptr, &tv);
                if (ready <= 0) break;  // timeout → sender got the ACK; done

                std::vector<char> retransmit(PACKET_SIZE, 0);
                struct sockaddr_in rAddr{};
                socklen_t rLen = sizeof(rAddr);
                ssize_t rn = recvfrom(socketFd,
                                      retransmit.data(), retransmit.size(), 0,
                                      reinterpret_cast<struct sockaddr*>(&rAddr),
                                      &rLen);
                if (rn < PACKET_SIZE) continue;

                // Re-ACK using lastAckedSeq (seq of the final accepted segment)
                std::vector<char> reAck(PACKET_SIZE, 0);

                char srcIPbuf[16]  = {};
                char srcPortbuf[6] = {};
                char dstIPbuf[16]  = {};
                char dstPortbuf[6] = {};
                std::memcpy(srcIPbuf,   retransmit.data() + OFF_SRC_IP,    16);
                std::memcpy(srcPortbuf, retransmit.data() + OFF_SRC_PORT,   6);
                std::memcpy(dstIPbuf,   retransmit.data() + OFF_DEST_IP,   16);
                std::memcpy(dstPortbuf, retransmit.data() + OFF_DEST_PORT,  6);

                std::memcpy(&reAck[OFF_SRC_IP],    dstIPbuf,   16);
                std::memcpy(&reAck[OFF_SRC_PORT],  dstPortbuf,  6);
                std::memcpy(&reAck[OFF_DEST_IP],   srcIPbuf,   16);
                std::memcpy(&reAck[OFF_DEST_PORT], srcPortbuf,  6);

                reAck[OFF_SEG + SEG_SEQ]     = static_cast<char>(lastAckedSeq);
                reAck[OFF_SEG + SEG_ACK]     = static_cast<char>(lastAckedSeq);
                reAck[OFF_SEG + SEG_CORRUPT] = 0;
                reAck[OFF_SEG + SEG_LAST]    = 0;
                reAck[OFF_SEG + SEG_LEN]     = 0;

                sendto(socketFd,
                       reAck.data(), reAck.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&rAddr),
                       rLen);
                std::cout << "[Receiver] Re-ACK " << lastAckedSeq
                          << " sent for last-segment retransmission.\n";
            }
            break;
        }
    }

    return message;
}