#pragma once

#include <experimental/optional>
#include <functional>
#include <iostream>

#include "io/network/network_endpoint.hpp"

namespace io::network {

/**
 * This class creates a network socket.
 * It is used to connect/bind/listen on a NetworkEndpoint (address + port).
 * It has wrappers for setting network socket flags and wrappers for
 * reading/writing data from/to the socket.
 */
class Socket {
 public:
  Socket() = default;
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&);
  Socket &operator=(Socket &&);
  ~Socket();

  /**
   * Closes the socket if it is open.
   */
  void Close();

  /**
   * Checks whether the socket is open.
   *
   * @return socket open status:
   *             true if the socket is open
   *             false if the socket is closed
   */
  bool IsOpen() const;

  /**
   * Connects the socket to the specified endpoint.
   *
   * @param endpoint NetworkEndpoint to which to connect to
   *
   * @return connection success status:
   *             true if the connect succeeded
   *             false if the connect failed
   */
  bool Connect(const NetworkEndpoint &endpoint);

  /**
   * Binds the socket to the specified endpoint.
   *
   * @param endpoint NetworkEndpoint to which to bind to
   *
   * @return bind success status:
   *             true if the bind succeeded
   *             false if the bind failed
   */
  bool Bind(const NetworkEndpoint &endpoint);

  /**
   * Start listening on the bound socket.
   *
   * @param backlog maximum number of pending connections in the connection
   *                queue
   *
   * @return listen success status:
   *             true if the listen succeeded
   *             false if the listen failed
   */
  bool Listen(int backlog);

  /**
   * Accepts a new connection.
   * This function accepts a new connection on a listening socket.
   *
   * @return socket if accepted, nullopt otherwise.
   */
  std::experimental::optional<Socket> Accept();

  /**
   * Sets the socket to non-blocking.
   *
   * @return set non-blocking success status:
   *             true if the socket was successfully set to non-blocking
   *             false if the socket was not set to non-blocking
   */
  bool SetNonBlocking();

  /**
   * Enables TCP keep-alive on the socket.
   *
   * @return enable keep-alive success status:
   *             true if keep-alive was successfully enabled on the socket
   *             false if keep-alive was not enabled
   */
  bool SetKeepAlive();

  /**
   * Enables TCP no_delay on the socket.
   * When enabled, the socket doesn't wait for an ACK of every data packet
   * before sending the next packet.
   *
   * @return enable no_delay success status:
   *             true if no_delay was successfully enabled on the socket
   *             false if no_delay was not enabled
   */
  bool SetNoDelay();

  /**
   * Sets the socket timeout.
   *
   * @param sec timeout seconds value
   * @param usec timeout microseconds value
   * @return set socket timeout status:
   *             true if the timeout was successfully set to
   *                 sec seconds + usec microseconds
   *             false if the timeout was not set
   */
  bool SetTimeout(long sec, long usec);

  /**
   * Returns the socket file descriptor.
   */
  int fd() const { return socket_; }

  /**
   * Returns the currently active endpoint of the socket.
   */
  const NetworkEndpoint &endpoint() const { return endpoint_; }

  /**
   * Write data to the socket.
   * Theese functions guarantee that all data will be written.
   *
   * @param data uint8_t* to data that should be written
   * @param len length of char* or uint8_t* data
   * @param keep_retrying while function executes to true socket will retry to
   * write data if nonterminal error occurred on socket (EAGAIN, EWOULDBLOCK,
   * EINTR)... useful if socket is in nonblocking mode or timeout is set on a
   * socket. By default Write doesn't retry if any error occurrs.
   *
   * TODO: Logic for retrying can be in derived class or in a wrapper of this
   * class, unfortunately from current return value we don't know what error
   * occured nor how much data was written.
   *
   * @return write success status:
   *             true if write succeeded
   *             false if write failed
   */
  bool Write(const uint8_t *data, size_t len,
             const std::function<bool()> &keep_retrying = [] { return false; });

  /**
   * Read data from the socket.
   * This function is a direct wrapper for the read function.
   *
   * @param buffer pointer to the read buffer
   * @param len length of the read buffer
   *
   * @return read success status:
   *             > 0 if data was read, means number of read bytes
   *             == 0 if the client closed the connection
   *             < 0 if an error has occurred
   */
  // TODO: Return type should be something like StatusOr<int> which would return
  // number of read bytes if read succeeded and error code and error message
  // otherwise (deduced from errno). We can implement that type easily on top of
  // std::variant once c++17 becomes available in memgraph.
  int Read(void *buffer, size_t len);

 private:
  Socket(int fd, const NetworkEndpoint &endpoint)
      : socket_(fd), endpoint_(endpoint) {}

  int socket_ = -1;
  NetworkEndpoint endpoint_;
};
}
