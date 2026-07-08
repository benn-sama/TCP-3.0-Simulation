/*
 * File:    rdtSender.cpp
 * Purpose: Implementation of the RDT 3.0 (Alternating Bit Protocol) sender.
 *          Segments messages into 10-byte packets (network header + payload),
 *          sends them one at a time to the network program via UDP, and handles
 *          timeouts and ACKs using select(). Sequence numbers alternate
 *          between 0 and 1 per the rdt 3.0 protocol.
 * Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#include "RDTSender.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ─── Packet layout constants ─────────────────────────────────────────────────
// Total UDP payload = 54 bytes
//   Network header  : 44 bytes
//     src IP        : bytes  0–15  (16 bytes, null-terminated string)
//     src port      : bytes 16–21  ( 6 bytes, null-terminated string)
//     dest IP       : bytes 22–37  (16 bytes, null-terminated string)
//     dest port     : bytes 38–43  ( 6 bytes, null-terminated string)
//   Transport segment: 10 bytes (bytes 44–53)
//     [44] seq_num  : 0 or 1
//     [45] ack_num  : 0 or 1  (sender sets equal to seq_num)
//     [46] corrupt  : 0 = clean, 1 = corrupt  (sender always sends 0)
//     [47] is_last  : 1 if this is the final segment of the message
//     [48] data_len : number of valid payload bytes in [49–53]
//     [49–53]       : payload (up to 5 bytes)

static const int PACKET_SIZE   = 54;  // total UDP payload size
static const int PAYLOAD_BYTES = 5;   // usable data bytes per segment
static const int TIMEOUT_SEC   = 2;   // retransmit timeout in seconds

// Field offsets within the 54-byte packet (network header)
static const int OFF_SRC_IP    =  0;
static const int OFF_SRC_PORT  = 16;
static const int OFF_DEST_IP   = 22;
static const int OFF_DEST_PORT = 38;
static const int OFF_SEG       = 44;  // start of transport segment

// Offsets within the transport segment (relative to OFF_SEG)
static const int SEG_SEQ     = 0;
static const int SEG_ACK     = 1;
static const int SEG_CORRUPT = 2;
static const int SEG_LAST    = 3;
static const int SEG_LEN     = 4;
static const int SEG_DATA    = 5;

// ─── Helper: resolve hostname to dotted-decimal IP string ────────────────────
static std::string resolveHost(const std::string& host) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return host;  // fall back to whatever was passed in

    char buf[INET_ADDRSTRLEN] = {};
    getnameinfo(res->ai_addr, res->ai_addrlen,
                buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST);
    freeaddrinfo(res);
    return std::string(buf);
}

// ─── Helper: build the 54-byte packet ────────────────────────────────────────
/*
 * Assembles a full 54-byte UDP payload:
 *   - network header  (src IP, src port, dest IP, dest port as strings)
 *   - transport segment (seq, ack, corrupt, is_last, data_len, data)
 *
 * @param srcIP    - sender's IP string
 * @param srcPort  - sender's bound port
 * @param destIP   - receiver's IP string  (written into header for network)
 * @param destPort - receiver's port       (written into header for network)
 * @param seqNum   - current sequence number (0 or 1)
 * @param isLast   - true if this is the final segment
 * @param data     - pointer to payload bytes
 * @param dataLen  - number of valid payload bytes (max PAYLOAD_BYTES)
 */
static std::vector<char> buildPacket(const std::string& srcIP,
                                     int                srcPort,
                                     const std::string& destIP,
                                     int                destPort,
                                     int                seqNum,
                                     bool               isLast,
                                     const char*        data,
                                     int                dataLen)
{
    std::vector<char> pkt(PACKET_SIZE, 0);

    // ── Network header ────────────────────────────────────────────────────
    std::string srcPortStr  = std::to_string(srcPort);
    std::string destPortStr = std::to_string(destPort);

    std::memcpy(&pkt[OFF_SRC_IP],    srcIP.c_str(),
                std::min((int)srcIP.size(),       15));
    std::memcpy(&pkt[OFF_SRC_PORT],  srcPortStr.c_str(),
                std::min((int)srcPortStr.size(),   5));
    std::memcpy(&pkt[OFF_DEST_IP],   destIP.c_str(),
                std::min((int)destIP.size(),      15));
    std::memcpy(&pkt[OFF_DEST_PORT], destPortStr.c_str(),
                std::min((int)destPortStr.size(),  5));

    // ── Transport segment ─────────────────────────────────────────────────
    pkt[OFF_SEG + SEG_SEQ]     = static_cast<char>(seqNum);
    pkt[OFF_SEG + SEG_ACK]     = static_cast<char>(seqNum); // echoed by receiver
    pkt[OFF_SEG + SEG_CORRUPT] = 0;                          // sender always sends clean
    pkt[OFF_SEG + SEG_LAST]    = isLast ? 1 : 0;
    pkt[OFF_SEG + SEG_LEN]     = static_cast<char>(dataLen);

    if (dataLen > 0)
        std::memcpy(&pkt[OFF_SEG + SEG_DATA], data,
                    std::min(dataLen, PAYLOAD_BYTES));

    return pkt;
}

// ─── Helper: validate a received ACK packet ───────────────────────────────────
/*
 * Checks whether the ACK packet is (a) not corrupt and (b) carries the
 * expected sequence number.
 *
 * @param pkt         - the received 54-byte packet
 * @param expectedSeq - the sequence number we are waiting to be ACK'd
 *
 * @return true if the ACK is valid and matches expectedSeq
 */
static bool isValidAck(const std::vector<char>& pkt, int expectedSeq) {
    if ((int)pkt.size() < PACKET_SIZE) return false;

    int corruptFlag = static_cast<unsigned char>(pkt[OFF_SEG + SEG_CORRUPT]);
    int ackNum      = static_cast<unsigned char>(pkt[OFF_SEG + SEG_ACK]);

    if (corruptFlag != 0) {
        std::cout << "[Sender] Corrupt ACK received – will retransmit.\n";
        return false;
    }
    return (ackNum == expectedSeq);
}

// ─── Constructor ─────────────────────────────────────────────────────────────
RDTSender::RDTSender(int                localPort,
                     const std::string& netwHost,
                     int                netwPort,
                     const std::string& destHost,
                     int                destPort)
    : socketFd(-1),
      localPort(localPort),
      netwHost(netwHost),
      netwPort(netwPort),
      destHost(destHost),
      destPort(destPort)
{
    // ── Create UDP socket ─────────────────────────────────────────────────
    socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        std::cerr << "[Sender] socket() failed: " << strerror(errno) << "\n";
        return;
    }

    // ── Bind to local port ────────────────────────────────────────────────
    struct sockaddr_in localAddr{};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_port        = htons(static_cast<uint16_t>(localPort));
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socketFd,
             reinterpret_cast<struct sockaddr*>(&localAddr),
             sizeof(localAddr)) < 0) {
        std::cerr << "[Sender] bind() failed: " << strerror(errno) << "\n";
        close(socketFd);
        socketFd = -1;
        return;
    }

    // ── Print hostname and bound port (spec requirement) ──────────────────
    struct sockaddr_in boundAddr{};
    socklen_t addrLen = sizeof(boundAddr);
    getsockname(socketFd,
                reinterpret_cast<struct sockaddr*>(&boundAddr),
                &addrLen);

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));

    std::cout << "[Sender] Host: " << hostname
              << "  Port: " << ntohs(boundAddr.sin_port) << "\n";
}

// ─── Destructor ───────────────────────────────────────────────────────────────
RDTSender::~RDTSender() {
    if (socketFd >= 0) {
        close(socketFd);
        socketFd = -1;
    }
}

// ─── sendMessage ──────────────────────────────────────────────────────────────
/*
 * Implements the rdt 3.0 sender protocol:
 *
 *   For each segment of the message:
 *     1. Build the 54-byte packet (network header + transport segment).
 *     2. sendto() the network program.
 *     3. Use select() to wait for either:
 *          a. Valid ACK with correct sequence number → advance, flip seq bit.
 *          b. Corrupt or wrong-sequence ACK          → retransmit.
 *          c. Timeout (TIMEOUT_SEC seconds)          → retransmit.
 *
 * @param message - the full text message to be delivered to the receiver
 *
 * @return 0 on success; -1 on error
 */
int RDTSender::sendMessage(const std::string& message) {
    if (socketFd < 0) {
        std::cerr << "[Sender] Socket is not open.\n";
        return -1;
    }
    if (message.empty()) {
        std::cerr << "[Sender] Empty message – nothing to send.\n";
        return -1;
    }

    // ── Resolve addresses ─────────────────────────────────────────────────
    // Network program: the only host we directly sendto()
    std::string netwIP = resolveHost(netwHost);

    struct sockaddr_in netwAddr{};
    netwAddr.sin_family = AF_INET;
    netwAddr.sin_port   = htons(static_cast<uint16_t>(netwPort));
    inet_pton(AF_INET, netwIP.c_str(), &netwAddr.sin_addr);

    // Receiver IP: written into the network header so the network
    // knows where to forward each packet
    std::string destIP = resolveHost(destHost);

    // Our own IP: written into the network header src fields
    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));
    std::string srcIP = resolveHost(std::string(hostname));

    // ── Segment the message into PAYLOAD_BYTES-byte chunks ────────────────
    std::vector<std::string> segments;
    for (int i = 0; i < (int)message.size(); i += PAYLOAD_BYTES)
        segments.push_back(message.substr(i, PAYLOAD_BYTES));

    std::cout << "[Sender] Sending " << segments.size()
              << " segment(s) for message: \"" << message << "\"\n";

    int seqNum = 0;  // alternating bit, starts at 0

    for (int idx = 0; idx < (int)segments.size(); ++idx) {
        const std::string& seg    = segments[idx];
        bool               isLast = (idx == (int)segments.size() - 1);

        // Build once; the same packet is retransmitted on timeout or bad ACK
        std::vector<char> pkt = buildPacket(srcIP, localPort,
                                            destIP, destPort,
                                            seqNum, isLast,
                                            seg.c_str(),
                                            static_cast<int>(seg.size()));
        bool acked = false;
        while (!acked) {
            // ── Transmit segment ──────────────────────────────────────────
            ssize_t sent = sendto(socketFd,
                                  pkt.data(), pkt.size(), 0,
                                  reinterpret_cast<struct sockaddr*>(&netwAddr),
                                  sizeof(netwAddr));
            if (sent < 0) {
                std::cerr << "[Sender] sendto() failed: " << strerror(errno) << "\n";
                return -1;
            }
            std::cout << "[Sender] Sent segment " << idx
                      << "  seq=" << seqNum
                      << "  last=" << isLast
                      << "  bytes=" << seg.size() << "\n";

            // ── Wait for ACK or timeout via select() ──────────────────────
            fd_set readFds;
            FD_ZERO(&readFds);
            FD_SET(socketFd, &readFds);

            struct timeval tv;
            tv.tv_sec  = TIMEOUT_SEC;
            tv.tv_usec = 0;

            int ready = select(socketFd + 1, &readFds, nullptr, nullptr, &tv);
            if (ready < 0) {
                std::cerr << "[Sender] select() error: " << strerror(errno) << "\n";
                return -1;
            }

            if (ready == 0) {
                // ── Timeout event: retransmit the current segment ─────────
                std::cout << "[Sender] Timeout on segment " << idx
                          << " (seq=" << seqNum << ") – retransmitting.\n";
                continue;
            }

            // ── ACK arrived: read and validate it ────────────────────────
            std::vector<char> ackBuf(PACKET_SIZE, 0);
            struct sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);

            ssize_t n = recvfrom(socketFd,
                                 ackBuf.data(), ackBuf.size(), 0,
                                 reinterpret_cast<struct sockaddr*>(&fromAddr),
                                 &fromLen);
            if (n < 0) {
                std::cerr << "[Sender] recvfrom() failed: " << strerror(errno) << "\n";
                return -1;
            }

            if (isValidAck(ackBuf, seqNum)) {
                std::cout << "[Sender] ACK " << seqNum
                          << " received for segment " << idx << ".\n";
                acked  = true;
                seqNum ^= 1;  // flip alternating bit: 0→1 or 1→0
            } else {
                std::cout << "[Sender] Wrong or corrupt ACK for segment "
                          << idx << " (seq=" << seqNum << ") – retransmitting.\n";
            }
        }
    }

    std::cout << "[Sender] Message delivered successfully.\n";
    return 0;
}