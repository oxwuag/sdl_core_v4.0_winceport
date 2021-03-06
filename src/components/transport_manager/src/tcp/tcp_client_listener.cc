/*
 *
 * Copyright (c) 2013, Ford Motor Company
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided with the
 * distribution.
 *
 * Neither the name of the Ford Motor Company nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "transport_manager/tcp/tcp_client_listener.h"

#if defined(OS_WIN32) || defined(OS_WINCE)
#include <memory.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <MSTcpIP.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#ifdef __linux__
#  include <linux/tcp.h>
#else  // __linux__
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <netinet/tcp_var.h>
#endif  // __linux__
#endif
#include <sstream>

#include "utils/logger.h"
#include "utils/threads/thread.h"
#include "transport_manager/transport_adapter/transport_adapter_controller.h"
#include "transport_manager/tcp/tcp_device.h"
#include "transport_manager/tcp/tcp_socket_connection.h"


namespace transport_manager {
namespace transport_adapter {

CREATE_LOGGERPTR_GLOBAL(logger_, "TransportManager")

#if defined(OS_WIN32) || defined(OS_WINCE)
	typedef int socklen_t;
#endif

TcpClientListener::TcpClientListener(TransportAdapterController* controller,
                                     const uint16_t port,
                                     const bool enable_keepalive)
    : port_(port),
      enable_keepalive_(enable_keepalive),
      controller_(controller),
      thread_(0),
      socket_(-1),
      thread_stop_requested_(false) {
  thread_ = threads::CreateThread("TcpClientListener",
                                  new ListeningThreadDelegate(this));
}

TransportAdapter::Error TcpClientListener::Init() {
  LOG4CXX_AUTO_TRACE(logger_);
  thread_stop_requested_ = false;

#if defined(OS_WIN32) || defined(OS_WINCE)
  WSADATA WSAData;
  if (WSAStartup(MAKEWORD(2, 0), &WSAData) != 0)
  {
	  TransportAdapter::FAIL;
  }
#endif

  socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == socket_) {
    LOG4CXX_ERROR_WITH_ERRNO(logger_, "Failed to create socket");
    return TransportAdapter::FAIL;
  }

  sockaddr_in server_address = { 0 };
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(port_);
  server_address.sin_addr.s_addr = INADDR_ANY;

#if defined(OS_WIN32) || defined(OS_WINCE)
  char optval = 1;
#else
  int optval = 1;
#endif
  setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  if (bind(socket_, reinterpret_cast<sockaddr*>(&server_address),
           sizeof(server_address)) != 0) {
    LOG4CXX_ERROR_WITH_ERRNO(logger_, "bind() failed");
    return TransportAdapter::FAIL;
  }

  const int kBacklog = 128;
  if (0 != listen(socket_, kBacklog)) {
    LOG4CXX_ERROR_WITH_ERRNO(logger_, "listen() failed");
    return TransportAdapter::FAIL;
  }
  return TransportAdapter::OK;
}

void TcpClientListener::Terminate() {
  LOG4CXX_AUTO_TRACE(logger_);
  if (socket_ == -1) {
    LOG4CXX_WARN(logger_, "Socket has been closed");
    return;
  }

#if defined(OS_WIN32) || defined(OS_WINCE)
  if (shutdown(socket_, 2) != 0) {
	  LOG4CXX_ERROR_WITH_ERRNO(logger_, "Failed to shutdown socket");
  }
  if (closesocket(socket_) != 0) {
	  LOG4CXX_ERROR_WITH_ERRNO(logger_, "Failed to close socket");
  }
#else
  if (shutdown(socket_, SHUT_RDWR) != 0) {
    LOG4CXX_ERROR_WITH_ERRNO(logger_, "Failed to shutdown socket");
  }
  if (close(socket_) != 0) {
    LOG4CXX_ERROR_WITH_ERRNO(logger_, "Failed to close socket");
  }
#endif

  socket_ = -1;
}

bool TcpClientListener::IsInitialised() const {
  return thread_;
}

TcpClientListener::~TcpClientListener() {
  LOG4CXX_AUTO_TRACE(logger_);
  StopListening();
  delete thread_->delegate();
  threads::DeleteThread(thread_);
  Terminate();
}

void SetKeepaliveOptions(const int fd) {
  LOG4CXX_AUTO_TRACE(logger_);
  LOG4CXX_DEBUG(logger_, "fd: " << fd);
  int yes = 1;
#if defined(OS_WIN32) || defined(OS_WINCE)
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&yes, sizeof(yes));

	tcp_keepalive inKeepAlive = { 0 };	// in parameter
	unsigned long ulInLen = sizeof(tcp_keepalive);
	tcp_keepalive outKeepAlive = { 0 }; // out parameter
	unsigned long ulOutLen = sizeof(tcp_keepalive);
	unsigned long ulBytesReturn = 0;

	inKeepAlive.onoff = RCVALL_ON;
	inKeepAlive.keepaliveinterval = 5000;
	inKeepAlive.keepalivetime = 1000;
	if (WSAIoctl((unsigned int)fd, SIO_KEEPALIVE_VALS,
		(LPVOID)&inKeepAlive, ulInLen,
		(LPVOID)&outKeepAlive, ulOutLen,
		&ulBytesReturn, NULL, NULL) == SOCKET_ERROR)
	{
		printf("WSAIoctl failed. error code(%d)!\n", WSAGetLastError());
		return;
	}
#else
  int keepidle = 3;  // 3 seconds to disconnection detecting
  int keepcnt = 5;
  int keepintvl = 1;
#ifdef __linux__
  int user_timeout = 7000;  // milliseconds
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
  setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout,
             sizeof(user_timeout));
#elif defined(__QNX__)  // __linux__
  // TODO(KKolodiy): Out of order!
  const int kMidLength = 4;
  int mib[kMidLength];

  mib[0] = CTL_NET;
  mib[1] = AF_INET;
  mib[2] = IPPROTO_TCP;
  mib[3] = TCPCTL_KEEPIDLE;
  sysctl(mib, kMidLength, NULL, NULL, &keepidle, sizeof(keepidle));

  mib[0] = CTL_NET;
  mib[1] = AF_INET;
  mib[2] = IPPROTO_TCP;
  mib[3] = TCPCTL_KEEPCNT;
  sysctl(mib, kMidLength, NULL, NULL, &keepcnt, sizeof(keepcnt));

  mib[0] = CTL_NET;
  mib[1] = AF_INET;
  mib[2] = IPPROTO_TCP;
  mib[3] = TCPCTL_KEEPINTVL;
  sysctl(mib, kMidLength, NULL, NULL, &keepintvl, sizeof(keepintvl));

  struct timeval tval = {0};
  tval.tv_sec = keepidle;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &tval, sizeof(tval));
#endif  // __QNX__
#endif
}

void TcpClientListener::Loop() {
  LOG4CXX_AUTO_TRACE(logger_);
  while (!thread_stop_requested_) {
    sockaddr_in client_address;
    socklen_t client_address_size = sizeof(client_address);
    const int connection_fd = accept(socket_,
                                     (struct sockaddr*) &client_address,
                                     &client_address_size);
    if (thread_stop_requested_) {
      LOG4CXX_DEBUG(logger_, "thread_stop_requested_");
#if defined(OS_WIN32) || defined(OS_WINCE)
      closesocket(connection_fd);
#else
      close(connection_fd);
#endif
      break;
    }

    if (connection_fd < 0) {
      LOG4CXX_ERROR_WITH_ERRNO(logger_, "accept() failed");
      continue;
    }

    if (AF_INET != client_address.sin_family) {
      LOG4CXX_DEBUG(logger_, "Address of connected client is invalid");
#if defined(OS_WIN32) || defined(OS_WINCE)
      closesocket(connection_fd);
#else
      close(connection_fd);
#endif
      continue;
    }

    char device_name[32];
    strncpy(device_name, inet_ntoa(client_address.sin_addr),
            sizeof(device_name) / sizeof(device_name[0]));
    LOG4CXX_INFO(logger_, "Connected client " << device_name);

    if (enable_keepalive_) {
      SetKeepaliveOptions(connection_fd);
    }

    TcpDevice* tcp_device = new TcpDevice(client_address.sin_addr.s_addr,
                                          device_name);
    DeviceSptr device = controller_->AddDevice(tcp_device);
    tcp_device = static_cast<TcpDevice*>(device.get());
    const ApplicationHandle app_handle = tcp_device->AddIncomingApplication(
        connection_fd);

    TcpSocketConnection* connection(
        new TcpSocketConnection(device->unique_device_id(), app_handle,
                                controller_));
    connection->set_socket(connection_fd);
    const TransportAdapter::Error error = connection->Start();
    if (error != TransportAdapter::OK) {
      delete connection;
    }
  }
}

void TcpClientListener::StopLoop() {
  LOG4CXX_AUTO_TRACE(logger_);
  thread_stop_requested_ = true;
  // We need to connect to the listening socket to unblock accept() call
  int byesocket = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in server_address = { 0 };
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(port_);
  server_address.sin_addr.s_addr = INADDR_ANY;
  connect(byesocket, reinterpret_cast<sockaddr*>(&server_address),
          sizeof(server_address));
#if defined(OS_WIN32) || defined(OS_WINCE)
  shutdown(byesocket, 2);
  closesocket(byesocket);
#else
  shutdown(byesocket, SHUT_RDWR);
  close(byesocket);
#endif
}

TransportAdapter::Error TcpClientListener::StartListening() {
  LOG4CXX_AUTO_TRACE(logger_);
  if (thread_->is_running()) {
    LOG4CXX_WARN(logger_,
                 "TransportAdapter::BAD_STATE. Listener has already been started");
    return TransportAdapter::BAD_STATE;
  }

  if (!thread_->start()) {
    LOG4CXX_ERROR(logger_, "Tcp client listener thread start failed");
    return TransportAdapter::FAIL;
  }
  LOG4CXX_INFO(logger_, "Tcp client listener has started successfully");
  return TransportAdapter::OK;
}

void TcpClientListener::ListeningThreadDelegate::exitThreadMain() {
  parent_->StopLoop();
}

void TcpClientListener::ListeningThreadDelegate::threadMain() {
  parent_->Loop();
}

TcpClientListener::ListeningThreadDelegate::ListeningThreadDelegate(
    TcpClientListener* parent)
    : parent_(parent) {
}

TransportAdapter::Error TcpClientListener::StopListening() {
  LOG4CXX_AUTO_TRACE(logger_);
  if (!thread_->is_running()) {
    LOG4CXX_DEBUG(logger_, "TcpClientListener is not running now");
    return TransportAdapter::BAD_STATE;
  }

  thread_->join();

  LOG4CXX_INFO(logger_, "Tcp client listener has stopped successfully");
  return TransportAdapter::OK;
}

}  // namespace transport_adapter
}  // namespace transport_manager