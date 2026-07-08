/**
 * This file describes the class implementing an RDT receiver.
 *
 * @author Thomas Reichherzer
 * @date 3/17/2026
 * @info Systems and Networks II
 * @info Project 3
 *
 */

#ifndef RDT_RECEIVER_HPP
#define RDT_RECEIVER_HPP

#include <string>

/**
 * The class models an RDT 3.0 receiver that receives a message from an RDT 3.0 sender.
 */
class RDTReceiver {

public:

    /** 
     * Constructs an RDTReceiver to receive messages on the specified port.
     *
     * @param port - the port on which the receiver listens
     */
     explicit RDTReceiver(int port);

     /**
      * Destructs the RDTReceiver freing up all resources allocated by the object of the class.
      */
    ~RDTReceiver();



    /**
     * Receives a message from an RDT sender on a specified port.
     *
     * @return the complete text message received; empty string indicates error
     */
    std::string receiveMessage();

private:

    int socketFd;   // the socket on which the message is received
    int port;       // the port on which the socket is bound
};

#endif // RDT_RECEIVER_HPP