// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Host test of src/ps5rt_netdb.cpp. host_test.sh builds it for Linux and renames its functions to t_* (objcopy), so
// they don't meet glibc's own; the console's resolver is faked below (same functions, same arguments), and records
// how it is called. The cases are the ones the ports' network code meets (libsmb2, libnfs, netfs, the helper).

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
int t_getaddrinfo(const char *, const char *, const addrinfo *, addrinfo **);
void t_freeaddrinfo(addrinfo *);
int t_getnameinfo(const sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
const char *t_gai_strerror(int);
protoent *t_getprotobyname(const char *);
protoent *t_getprotobynumber(int);
servent *t_getservbyname(const char *, const char *);
servent *t_getservbyport(int, const char *);
}

// ---- the fake resolver ----
namespace {
int pools_made, pool_failures_left = 1, resolvers_open, lookups, last_timeout, last_retries;
std::string last_name;
} // namespace

extern "C" {
int sceNetInit(void) { return 0; }
int sceNetPoolCreate(const char *, int size, int)
{
    if (pool_failures_left > 0)
    {
        --pool_failures_left;
        return -1;
    }
    ++pools_made;
    return size > 0 ? 7 : -1;
}
int sceNetResolverCreate(const char *, int pool, int)
{
    if (pool != 7)
        return -1;
    ++resolvers_open;
    return 3;
}
int sceNetResolverStartNtoa(int resolver, const char *name, std::uint32_t *address, int timeout_us, int retries, int)
{
    ++lookups;
    last_name = name;
    last_timeout = timeout_us;
    last_retries = retries;
    if (resolver != 3)
        return -1;
    if (std::strcmp(name, "nas.local") == 0)
    {
        *address = inet_addr("192.168.1.20");
        return 0;
    }
    if (std::strcmp(name, "zero.local") == 0)
    {
        *address = 0;
        return 0;
    }
    return static_cast<int>(0x80410000u | 0x1F);   // an SCE error (negative)
}
int sceNetResolverDestroy(int resolver)
{
    if (resolver == 3)
        --resolvers_open;
    return 0;
}
}

namespace {
int failures;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}

std::string addr_of(const addrinfo *ai)
{
    if (ai == nullptr || ai->ai_family != AF_INET || ai->ai_addr == nullptr || ai->ai_addrlen != sizeof(sockaddr_in))
        return "?";
    const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(ai->ai_addr);
    char text[32];
    inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
    return std::string(text) + ":" + std::to_string(ntohs(in->sin_port));
}

int gai(const char *node, const char *service, int flags, int family, int socktype, std::string &out,
        int *type = nullptr, int *proto = nullptr, std::string *canon = nullptr)
{
    addrinfo hints = {};
    hints.ai_flags = flags;
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    addrinfo *res = nullptr;
    const bool no_hints = flags == -1;
    const int rc = t_getaddrinfo(node, service, no_hints ? nullptr : &hints, &res);
    out = rc == 0 ? addr_of(res) : "";
    if (rc == 0 && res->ai_next != nullptr)
        out += " +more";
    if (rc == 0 && type)
        *type = res->ai_socktype;
    if (rc == 0 && proto)
        *proto = res->ai_protocol;
    if (rc == 0 && canon)
        *canon = res->ai_canonname ? res->ai_canonname : "";
    t_freeaddrinfo(res);
    return rc;
}

std::string gni(const sockaddr *sa, socklen_t len, int flags, int *rc, socklen_t hostlen = 64, socklen_t servlen = 16,
                bool host = true, bool serv = true)
{
    char h[64] = "", s[16] = "";
    *rc = t_getnameinfo(sa, len, host ? h : nullptr, host ? hostlen : 0, serv ? s : nullptr, serv ? servlen : 0, flags);
    return std::string(h) + "|" + s;
}
} // namespace

int main()
{
    std::string out;
    int type = 0, proto = 0, rc;

    // the resolver's pool can't be made: EAI_FAIL; the next call makes it
    check(gai("nas.local", nullptr, -1, 0, 0, out) == EAI_FAIL && pools_made == 0, "no resolver pool: EAI_FAIL");
    check(gai("nas.local", "445", -1, 0, 0, out, &type, &proto) == 0 && out == "192.168.1.20:445" && type == SOCK_STREAM &&
              proto == IPPROTO_TCP && pools_made == 1,
          "libsmb2: getaddrinfo(name, \"445\", NULL): the resolver's address, port 445, one SOCK_STREAM result");
    check(last_name == "nas.local" && last_timeout == 5000000 && last_retries == 2 && resolvers_open == 0,
          "... asked with 5 s, 2 retries; the resolver closed again");
    const int before = lookups;
    check(gai("192.168.1.10", nullptr, -1, 0, 0, out) == 0 && out == "192.168.1.10:0" && lookups == before,
          "libnfs: getaddrinfo(dotted quad, NULL, NULL): parsed, the resolver not asked");
    check(gai("10.0.0.5", "2049", 0, AF_INET, SOCK_DGRAM, out, &type, &proto) == 0 && out == "10.0.0.5:2049" &&
              type == SOCK_DGRAM && proto == IPPROTO_UDP,
          "libnfs: a port string with SOCK_DGRAM: UDP, port 2049");
    check(gai("nas.local", nullptr, AI_NUMERICHOST, AF_INET, SOCK_STREAM, out) == EAI_NONAME && lookups == before,
          "AI_NUMERICHOST with a name: EAI_NONAME, no lookup");
    check(gai("10.1.2.3", nullptr, AI_NUMERICHOST, 0, 0, out) == 0 && out == "10.1.2.3:0", "AI_NUMERICHOST, a dotted quad");
    for (const char *bad : {"01.2.3.4", "256.1.1.1", "1.2.3", "1.2.3.4.5", "1.2.3.4 ", "a.b.c.d"})
        check(gai(bad, nullptr, AI_NUMERICHOST, 0, 0, out) == EAI_NONAME, std::string("not a dotted quad: \"") + bad + "\"");
    check(gai("unknown.local", nullptr, -1, 0, 0, out) == EAI_NONAME && resolvers_open == 0,
          "a name the resolver doesn't know: EAI_NONAME");
    check(gai("zero.local", nullptr, -1, 0, 0, out) == EAI_NONAME, "the resolver answers 0.0.0.0: EAI_NONAME");
    check(gai("10.0.0.1", "microsoft-ds", 0, 0, SOCK_STREAM, out) == 0 && out == "10.0.0.1:445", "service name microsoft-ds: 445");
    check(gai("10.0.0.1", "nfs", 0, 0, SOCK_DGRAM, out) == 0 && out == "10.0.0.1:2049", "service name nfs (udp): 2049");
    check(gai("10.0.0.1", "sunrpc", 0, 0, 0, out) == 0 && out == "10.0.0.1:111", "service name sunrpc: 111");
    check(gai("10.0.0.1", "nfs", AI_NUMERICSERV, 0, 0, out) == EAI_NONAME, "AI_NUMERICSERV with a name: EAI_NONAME");
    check(gai("10.0.0.1", "bogus", 0, 0, 0, out) == EAI_SERVICE, "an unknown service: EAI_SERVICE");
    check(gai("10.0.0.1", "70000", 0, 0, 0, out) == EAI_SERVICE, "port 70000: EAI_SERVICE");
    check(gai("10.0.0.1", "0", 0, 0, 0, out) == 0 && out == "10.0.0.1:0", "port 0");
    check(gai(nullptr, "8080", AI_PASSIVE, 0, 0, out) == 0 && out == "0.0.0.0:8080", "no host, AI_PASSIVE: INADDR_ANY");
    check(gai(nullptr, "8080", 0, 0, 0, out) == 0 && out == "127.0.0.1:8080", "no host: loopback");
    check(gai(nullptr, nullptr, 0, 0, 0, out) == EAI_NONAME, "no host, no service: EAI_NONAME");
    check(gai("10.0.0.1", nullptr, 0, AF_INET6, 0, out) == EAI_FAMILY, "AF_INET6: EAI_FAMILY");
    check(gai("10.0.0.1", nullptr, 0x40000000, 0, 0, out) == EAI_BADFLAGS, "an unknown flag: EAI_BADFLAGS");
    check(gai("10.0.0.1", nullptr, 0, 0, SOCK_RAW, out) == EAI_SOCKTYPE, "SOCK_RAW: EAI_SOCKTYPE");
    std::string canon;
    check(gai("nas.local", nullptr, AI_CANONNAME, 0, 0, out, nullptr, nullptr, &canon) == 0 && canon == "nas.local",
          "AI_CANONNAME: the name");
    t_freeaddrinfo(nullptr);
    check(true, "freeaddrinfo(NULL)");

    sockaddr_in in = {};
    in.sin_family = AF_INET;
    in.sin_port = htons(2049);
    in.sin_addr.s_addr = inet_addr("10.0.0.5");
    const sockaddr *sa = reinterpret_cast<const sockaddr *>(&in);
    check(gni(sa, sizeof in, NI_NUMERICHOST, &rc, 64, 16, true, false) == "10.0.0.5|" && rc == 0,
          "libnfs: getnameinfo(NI_NUMERICHOST), no service buffer");
    check(gni(sa, sizeof in, NI_NUMERICHOST | NI_NUMERICSERV, &rc) == "10.0.0.5|2049" && rc == 0, "NI_NUMERICSERV: 2049");
    check(gni(sa, sizeof in, NI_NUMERICHOST, &rc) == "10.0.0.5|nfs" && rc == 0, "the service by name: nfs");
    in.sin_port = htons(137);
    check(gni(sa, sizeof in, NI_DGRAM, &rc) == "10.0.0.5|netbios-ns" && rc == 0, "NI_DGRAM: udp's name for 137");
    in.sin_port = htons(5555);
    check(gni(sa, sizeof in, 0, &rc) == "10.0.0.5|5555" && rc == 0, "a port with no name: the number");
    gni(sa, sizeof in, 0, &rc, 8);
    check(rc == EAI_OVERFLOW, "a host buffer too small: EAI_OVERFLOW");
    gni(sa, sizeof in, 0, &rc, 64, 4);
    check(rc == EAI_OVERFLOW, "a service buffer too small: EAI_OVERFLOW");
    gni(sa, sizeof in, NI_NAMEREQD, &rc);
    check(rc == EAI_NONAME, "NI_NAMEREQD (no reverse lookup): EAI_NONAME");
    gni(sa, sizeof in - 1, 0, &rc);
    check(rc == EAI_FAIL, "a short sockaddr: EAI_FAIL");
    gni(sa, sizeof in, 0, &rc, 64, 16, false, false);
    check(rc == EAI_NONAME, "neither host nor service asked: EAI_NONAME");
    sockaddr_in6 in6 = {};
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(445);
    inet_pton(AF_INET6, "2001:db8::1", &in6.sin6_addr);
    check(gni(reinterpret_cast<sockaddr *>(&in6), sizeof in6, NI_NUMERICSERV, &rc) == "2001:db8::1|445" && rc == 0,
          "IPv6: 2001:db8::1");
    inet_pton(AF_INET6, "::1", &in6.sin6_addr);
    check(gni(reinterpret_cast<sockaddr *>(&in6), sizeof in6, NI_NUMERICSERV, &rc) == "::1|445", "IPv6: ::1");
    inet_pton(AF_INET6, "fe80:0:0:1:0:0:0:2", &in6.sin6_addr);
    check(gni(reinterpret_cast<sockaddr *>(&in6), sizeof in6, NI_NUMERICSERV, &rc) == "fe80:0:0:1::2|445",
          "IPv6: the longest zero run compressed");
    sockaddr other = {};
    other.sa_family = AF_UNIX;
    gni(&other, sizeof other, 0, &rc);
    check(rc == EAI_FAMILY, "another family: EAI_FAMILY");

    bool texts = std::strcmp(t_gai_strerror(0), "Success") == 0;
    for (int code : {EAI_AGAIN, EAI_BADFLAGS, EAI_FAIL, EAI_FAMILY, EAI_MEMORY, EAI_NONAME, EAI_SERVICE, EAI_SOCKTYPE,
                     EAI_SYSTEM, EAI_OVERFLOW})
        texts = texts && std::strcmp(t_gai_strerror(code), "Unknown error") != 0;
    check(texts && std::strcmp(t_gai_strerror(12345), "Unknown error") == 0, "gai_strerror: a text for every code");

    const protoent *tcp = t_getprotobyname("tcp");
    check(tcp && tcp->p_proto == IPPROTO_TCP && t_getprotobyname("udp")->p_proto == IPPROTO_UDP &&
              !t_getprotobyname("xyz") && std::strcmp(t_getprotobynumber(IPPROTO_UDP)->p_name, "udp") == 0,
          "libsmb2: getprotobyname(\"tcp\"), \"udp\"; getprotobynumber");
    const servent *se = t_getservbyname("microsoft-ds", "tcp");
    check(se && se->s_port == htons(445) && std::strcmp(se->s_proto, "tcp") == 0 &&
              std::strcmp(t_getservbyport(htons(111), "udp")->s_name, "sunrpc") == 0 &&
              !t_getservbyname("nfs", "sctp"),
          "getservbyname / getservbyport");

    std::printf("test_netdb: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
