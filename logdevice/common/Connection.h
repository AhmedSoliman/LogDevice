/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/Request.h>

#include "logdevice/common/Address.h"
#include "logdevice/common/ClientID.h"
#include "logdevice/common/ProtocolHandler.h"
#include "logdevice/common/Socket.h"

namespace facebook { namespace logdevice {

class SocketAdapter;

/**
 * this will we a wrapper around our socket which knows about protocol and
 * serialization
 */
class Connection : public Socket {
 public:
  /**
   * Constructs a new Connection, to be connected to a LogDevice
   * server. The calling thread must be a Worker thread.
   *
   * @param server_name     id of server to connect to
   * @param type            type of socket
   * @param flow_group      traffic shaping state shared between sockets
   *                        with the same bandwidth constraints.
   * @params deps           SocketDependencies provides a way to callback into
   *                        higher layers and provides notification mechanism.
   *                        It depends on dependencies for stuff like Stats and
   *                        config and other data.
   *
   * @return  on success, a new fully constructed Connection is returned. It is
   *          expected that the Connection will be registered with the Worker's
   *          Sender under server_name. On failure throws ConstructorFailed and
   *          sets err to:
   *
   *     INVALID_THREAD  current thread is not running a Worker (debug build
   *                     asserts)
   *     NOTINCONFIG     server_name does not appear in cluster config
   *     INTERNAL        failed to initialize a libevent timer (unlikely)
   */
  Connection(NodeID server_name,
             SocketType type,
             ConnectionType conntype,
             FlowGroup& flow_group,
             std::unique_ptr<SocketDependencies> deps);

  Connection(NodeID server_name,
             SocketType type,
             ConnectionType conntype,
             FlowGroup& flow_group,
             std::unique_ptr<SocketDependencies> deps,
             std::unique_ptr<SocketAdapter> sock_adapter);

  /**
   * Constructs a new Connection from a TCP socket fd that was returned by
   * accept(). The thread must run a Worker. On success the socket is emplaced
   * on this Sender's .client_sockets_ map with client_name as key.
   *
   * @param fd        fd of the accepted socket. The caller passes
   *                  responsibility for closing fd to the constructor.
   * @param client_name local identifier assigned to this passively accepted
   *                    connection (aka "client address")
   * @param client_addr sockaddr we got from accept() for this client connection
   * @param conn_token  used to keep track of all accepted connections
   * @param type        type of socket
   * @param flow_group  traffic shaping state shared between sockets
   *                    with the same bandwidth constraints.
   * @params deps       SocketDependencies provides a way to callback into
   *                    higher layers and provides notification mechanism.
   *                    It depends on dependencies for stuff like Stats and
   *                    config and other data.
   *
   * @return  on success, a new fully constructed Connection is returned. On
   *          failure throws ConstructorFailed and sets err to:
   *
   *     INVALID_THREAD  current thread is not running a Worker (debug build
   *                     asserts)
   *     NOMEM           a libevent function could not allocate memory
   *     INTERNAL        failed to set fd non-blocking (unlikely) or failed to
   *                     initialize a libevent timer (unlikely).
   */
  Connection(int fd,
             ClientID client_name,
             const Sockaddr& client_addr,
             ResourceBudget::Token conn_token,
             SocketType type,
             ConnectionType conntype,
             FlowGroup& flow_group,
             std::unique_ptr<SocketDependencies> deps);

  Connection(int fd,
             ClientID client_name,
             const Sockaddr& client_addr,
             ResourceBudget::Token conn_token,
             SocketType type,
             ConnectionType conntype,
             FlowGroup& flow_group,
             std::unique_ptr<SocketDependencies> deps,
             std::unique_ptr<SocketAdapter> sock_adapter);

  /**
   * Disconnects, deletes the underlying bufferevent, and closes the TCP socket.
   */
  ~Connection() override;

  Connection(const Connection&) = delete;
  Connection(Connection&&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection& operator=(Connection&&) = delete;
  /**
   * Initiate an asynchronous connect and handshake on the socket. The socket's
   * .peer_name_ must resolve to an ip:port to which we can connect. Currently
   * this means that .peer_name_ must be a server address.
   *
   * @return  0 if connection was successfully initiated. -1 on failure, err
   *          is set to:
   *
   *    ALREADY         the socket is already in CONNECTING or HANDSHAKE
   *    ISCONN          the socket is CONNECTED
   *    UNREACHABLE     attempt to connect to a client. Reported for
   *                    disconnected client sockets.
   *    UNROUTABLE      the peer endpoint of a server socket has an IP address
   *                    to which there is no route. This may happen if a network
   *                    interface has been taken down, e.g., during system
   *                    shutdown.
   *    DISABLED        connection was not initiated because the server
   *                    is temporarily marked down (disabled) after a series
   *                    of unsuccessful connection attempts
   *    SYSLIMIT        out of file descriptors or ephemeral ports
   *    NOMEM           out of kernel memory for sockets, or malloc() failed
   *    INTERNAL        bufferevent unexpectedly failed to initiate connection,
   *                    unexpected error from socket(2).
   */
  int connect() override;

  Socket::SendStatus
  sendBuffer(std::unique_ptr<folly::IOBuf>&& buffer_chain) override;

  void close(Status reason) override;

  bool isClosed() const override;

  void setSocketAdapter(std::unique_ptr<SocketAdapter> adapter);

  void onBytesPassedToTCP(size_t nbytes_drained) override;

  size_t getBytesPending() const override;

 protected:
  folly::Future<Status> asyncConnect();
  void onConnected() override;
  int onReceived(ProtocolHeader ph, struct evbuffer* inbuf) override;
  /**
   * Called when connection timeout occurs. Either we could not establish the
   * TCP connection after multiple retries or the LD handshake did not complete
   * in time.
   */
  void onConnectTimeout() override;

  /**
   * Called when LD handshake doesn't complete in the allottted time.
   */
  void onHandshakeTimeout() override;

  /**
   * Called when the TCP connection could not be established in time.
   * If n_retries_left_ is positive, will try to connect again.
   */
  void onConnectAttemptTimeout() override;

  void onSent(std::unique_ptr<Envelope>,
              Status,
              Message::CompletionMethod =
                  Message::CompletionMethod::IMMEDIATE) override;

  void onError(short direction, int socket_errno) override;

  void onPeerClosed() override;

  std::unique_ptr<SocketAdapter> sock_;
  std::shared_ptr<ProtocolHandler> proto_handler_;

  // This is passed to WriteCallback to activate
  class SocketWriteCallback : public folly::AsyncSocket::WriteCallback {
   public:
    SocketWriteCallback(IProtocolHandler* conn = nullptr)
        : proto_handler_(conn) {}
    /**
     * writeSuccess() will be invoked when all of the data has been
     * successfully written.
     *
     * Note that this mainly signals that the buffer containing the data to
     * write is no longer needed and may be freed or re-used.  It does not
     * guarantee that the data has been fully transmitted to the remote
     * endpoint.  For example, on socket-based transports, writeSuccess() only
     * indicates that the data has been given to the kernel for eventual
     * transmission.
     */
    void writeSuccess() noexcept override {
      proto_handler_->notifyBytesWritten(0);
      ++num_success_;
    }

    /**
     * writeError() will be invoked if an error occurs writing the data.
     *
     * @param bytesWritten The number of bytes that were successfull
     * @param ex           An exception describing the error that occurred.
     */
    void writeErr(size_t /* bytesWritten */,
                  const folly::AsyncSocketException& ex) noexcept override {
      proto_handler_->notifyErrorOnSocket(ex);
    }

    size_t bufferedBytes() const {
      size_t buffered_bytes = 0;
      for (const size_t& len : chain_lengths_) {
        buffered_bytes += len;
      }
      return buffered_bytes;
    }

    IProtocolHandler* proto_handler_;
    std::deque<size_t> chain_lengths_;
    size_t num_success_{0};
  };

  SocketWriteCallback sock_write_cb_;

  std::unique_ptr<folly::IOBuf> sendChain_;
  void drainSendQueue();
};

}} // namespace facebook::logdevice
