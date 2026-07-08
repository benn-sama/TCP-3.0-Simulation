/**
 * This file describes the class implementing an RDT sender.
 *
 * @author Thomas Reichherzer
 * @date 3/17/2026
 * @info Systems and Networks II
 * @info Project 3
 *
 */

#ifndef RDT_SENDER_HPP
#define RDT_SENDER_HPP

#include <string>

/**
 * The class models an RDT 3.0 sender that sends a message to an RDT 3.0 receiver.
 */
class RDTSender {
    
public:

    /**
     * Constructs and RDTSender using a port to send a message, ports and hostnames of the network and the RDT receiver.
     *
     * @param localPort - the local port to bind the socket
     * @param netwHost  - the host running the network simulator
     * @param netwPort  - the port on which the network simulator listens
     * @param destHost  - the host running the receiver
     * @param destPort  - the port on which the receiver listens
     */
    RDTSender(int localPort,
              const std::string& netwHost,
              int netwPort,
              const std::string& destHost,
              int destPort);

    /**
      * Destructs the RDTSender freing up all resources allocated by the object of the class.
      */
    ~RDTSender();

    /**
     * Sends a message to an RDT receiver via a network simulator.
     * 
     * @param message   - the entire text message to be sent
     *
     * @return 0 if no error; otherwise, a negative error code
     */
    int sendMessage(const std::string& message);

private:
    int socketFd;         // the socket on which the message is received

    int localPort;        // the port on which the socket is bound

    std::string netwHost; // the host running the network simulator
    int netwPort;         // the port on which the network simulator listens

    std::string destHost; // the host running the receiver
    int destPort;         // the port on which the receiver listens
};

#endif // RDT_SENDER_HPP