/**
 * This file describes the class implementing an RDT network simulator.
 * The network forwards packets between an RDT sender and receiver via UDP,
 * and simulates packet loss, corruption, and delay. It also records and
 * periodically prints traffic statistics.
 * Author:  Chris Jackson, Ben Green
 * Course:  COP4635 Sys & Net II
 * Date:    Spring 2026
 */

#ifndef RDT_NETWORK_HPP
#define RDT_NETWORK_HPP

#include <string>

/**
 * The class is the network simulator sitting between an RDT 3.0 sender
 * and receiver. It receives packets from either side, optionally drops,
 * corrupts, or delays them, then forwards them to the correct destination
 * using the address information embedded in the packet's network header.
 */
class RDTNetwork {
public:
    /**
     * Constructs an RDTNetwork bound to a local port with
     * probabilities to simulate lost, delayed, and corrupt packets.
     *
     * @param localPort      - the local port to bind the UDP socket
     * @param lostPercent    - probability (0–100) that a packet is dropped
     * @param delayedPercent - probability (0–100) that a packet is delayed
     * @param errorPercent   - probability (0–100 that a packet is corrupted
     */
    RDTNetwork(int localPort,
               int lostPercent,
               int delayedPercent,
               int errorPercent);

    /**
     *  destuctor for freeing all resources allocated by the object.
     */
    ~RDTNetwork();

    /**
     * Starts the network forwarding loop. Receives packets from the sender
     * or receiver and forwards them to the correct destination as indicated
     * by the network header embedded in each packet. Simulates packet loss,
     * delay (via a detached thread), and corruption. Periodically prints
     * traffic statistics to standard output.
     */
    void run();

private:
    int socketFd;        // UDP socket used for all sends and receives
    int localPort;       // port on which the network socket is bound
    int lostPercent;     // percentage chance a packet is dropped  (0–100)
    int delayedPercent;  // percentage chance a packet is delayed  (0–100)
    int errorPercent;    // percentage chance a packet is corrupted (0–100)

    // traffic stats
    int totalReceived;   // total packets received by the network
    int totalForwarded;  // total packets successfully forwarded
    int totalDropped;    // total packets dropped (simulated loss)
    int totalDelayed;    // total packets delayed (simulated delay)
    int totalCorrupted;  // total packets corrupted (simulated error)

    /**
     * Forwards a packet to the destination address encoded in its network
     * header. Extracts the destination IP and port from the fixed-format
     * header and calls sendto().
     *
     * @param packet    - pointer to the 54-byte packet buffer
     * @param packetLen - length of the packet in bytes
     */
    void forwardPacket(const char* packet, int packetLen);

    /**
     * Simulates packet corruption by flipping the corrupt flag (byte 46)
     * in the transport segment of the packet.
     *
     * @param packet    - pointer to the 54-byte packet buffer
     * @param packetLen - length of the packet in bytes
     */
    void corruptPacket(char* packet, int packetLen);

    /**
     * Launches a detached thread that sleeps for a fixed delay then forwards
     * the packet, simulating slow network transport. The delay must be between
     * 1.5× and 2× the sender's retransmit timeout so that the sender's timer
     * fires before the delayed packet arrives.
     *
     * @param packet    - the 54-byte packet buffer (copied into the thread)
     * @param packetLen - length of the packet in bytes
     */
    void delayAndForward(const char* packet, int packetLen);

    /**
     * Prints a summary of traffic statistics to standard output. Called
     * periodically (e.g., every other packet received) so the user can
     * monitor network activity during the simulation.
     */
    void printStats() const;
};

#endif // RDT_NETWORK_HPP