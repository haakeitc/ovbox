#include "udpsocket.h"
#include "common.h"
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LISTEN_BACKLOG 512

const size_t pingbufsize(
    HEADERLEN + sizeof(std::chrono::high_resolution_clock::time_point) + 1);

udpsocket_t::udpsocket_t()
{
  bzero((char*)&serv_addr, sizeof(serv_addr));
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if(sockfd < 0)
    throw ErrMsg("Opening socket failed: ", errno);
  isopen = true;
}

udpsocket_t::~udpsocket_t()
{
  close();
}

void udpsocket_t::close()
{
  if(isopen)
    ::close(sockfd);
  isopen = false;
}

void udpsocket_t::destination(const char* host)
{
  struct hostent* server;
  server = gethostbyname(host);
  if(server == NULL)
    throw ErrMsg("No such host: " + std::string(hstrerror(h_errno)));
  bzero((char*)&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char*)server->h_addr, (char*)&serv_addr.sin_addr.s_addr,
        server->h_length);
}

port_t udpsocket_t::bind(port_t port, bool loopback)
{
  int optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval,
             sizeof(int));

  endpoint_t my_addr;
  memset(&my_addr, 0, sizeof(endpoint_t));
  /* Clear structure */
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons((unsigned short)port);
  if(loopback) {
    my_addr.sin_addr.s_addr = 0x0100007f;
  }
  if(::bind(sockfd, (struct sockaddr*)&my_addr, sizeof(endpoint_t)) == -1)
    throw ErrMsg("Binding the socket to port " + std::to_string(port) +
                     " failed: ",
                 errno);
  socklen_t addrlen(sizeof(endpoint_t));
  getsockname(sockfd, (struct sockaddr*)&my_addr, &addrlen);
  return ntohs(my_addr.sin_port);
}

size_t udpsocket_t::send(const char* buf, size_t len, int portno)
{
  if(portno == 0)
    return len;
  serv_addr.sin_port = htons(portno);
  return sendto(sockfd, buf, len, MSG_CONFIRM, (struct sockaddr*)&serv_addr,
                sizeof(serv_addr));
}

size_t udpsocket_t::send(const char* buf, size_t len, const endpoint_t& ep)
{
  return sendto(sockfd, buf, len, MSG_CONFIRM, (struct sockaddr*)&ep,
                sizeof(ep));
}

size_t udpsocket_t::recvfrom(char* buf, size_t len, endpoint_t& addr)
{
  memset(&addr, 0, sizeof(endpoint_t));
  addr.sin_family = AF_INET;
  socklen_t socklen(sizeof(endpoint_t));
  return ::recvfrom(sockfd, buf, len, 0, (struct sockaddr*)&addr, &socklen);
}

std::string addr2str(const struct in_addr& addr)
{
  return std::to_string(addr.s_addr & 0xff) + "." +
         std::to_string((addr.s_addr >> 8) & 0xff) + "." +
         std::to_string((addr.s_addr >> 16) & 0xff) + "." +
         std::to_string((addr.s_addr >> 24) & 0xff);
}

std::string ep2str(const endpoint_t& ep)
{
  return addr2str(ep.sin_addr) + "/" + std::to_string(ntohs(ep.sin_port));
}

ovbox_udpsocket_t::ovbox_udpsocket_t(secret_t secret) : secret(secret) {}

void ovbox_udpsocket_t::send_ping(callerid_t cid, const endpoint_t& ep)
{
  if(cid >= MAXEP)
    return;
  char buffer[pingbufsize];
  std::chrono::high_resolution_clock::time_point t1(
      std::chrono::high_resolution_clock::now());
  size_t n = packmsg(buffer, pingbufsize, secret, cid, PORT_PING, 0,
                     (const char*)(&t1), sizeof(t1));
  send(buffer, n, ep);
}

void ovbox_udpsocket_t::send_registration(callerid_t cid, epmode_t mode,
                                          port_t port)
{
  std::string rver(OVBOXVERSION);
  size_t buflen(HEADERLEN + rver.size() + 1);
  char buffer[buflen];
  size_t n(packmsg(buffer, buflen, secret, cid, PORT_REGISTER, mode,
                   rver.c_str(), rver.size() + 1));
  send(buffer, n, port);
}

char* ovbox_udpsocket_t::recv_sec_msg(char* inputbuf, size_t& ilen, size_t& len,
                                      callerid_t& cid, port_t& destport,
                                      sequence_t& seq, endpoint_t& addr)
{
  ilen = recvfrom(inputbuf, ilen, addr);
  if(ilen < HEADERLEN)
    return NULL;
  // check secret:
  if(msg_secret(inputbuf) != secret) {
    // log( 0, "invalid secret "+std::to_string(msg_secret(inputbuf)) +" from
    // "+ep2str(addr));
    return NULL;
  }
  cid = msg_callerid(inputbuf);
  destport = msg_port(inputbuf);
  seq = msg_seq(inputbuf);
  len = ilen - HEADERLEN;
  return &(inputbuf[HEADERLEN]);
}

std::string getmacaddr()
{
  std::string retv;
  struct ifreq ifr;
  struct ifconf ifc;
  char buf[1024];
  int success = 0;

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if(sock == -1) { /* handle error*/
    return retv;
  };

  ifc.ifc_len = sizeof(buf);
  ifc.ifc_buf = buf;
  if(ioctl(sock, SIOCGIFCONF, &ifc) == -1) { /* handle error */
    return retv;
  }

  struct ifreq* it = ifc.ifc_req;
  const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

  for(; it != end; ++it) {
    strcpy(ifr.ifr_name, it->ifr_name);
    if(ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
      if(!(ifr.ifr_flags & IFF_LOOPBACK)) { // don't count loopback
        if(ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
          success = 1;
          break;
        }
      }
    } else { /* handle error */
      return retv;
    }
  }

  unsigned char mac_address[6];

  if(success) {
    memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
    char ctmp[1024];
    sprintf(ctmp, "%02x%02x%02x%02x%02x%02x", mac_address[0], mac_address[1],
            mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
    retv = ctmp;
  }
  return retv;
}
