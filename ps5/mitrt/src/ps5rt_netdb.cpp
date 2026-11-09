// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Network database functions (POSIX <netdb.h>) for a native PS5 title, on the system's resolver (libSceNet's
// sceNetResolver*): getaddrinfo, freeaddrinfo, getnameinfo, gai_strerror, getprotobyname, getprotobynumber,
// getservbyname, getservbyport. Linked ahead of the payload SDK's libc.a, they keep its netdb.o (GPL) out of the app.
//
// What it does:
//   - IPv4 only (the resolver's sceNetResolverStartNtoa gives one IPv4 address). A numeric host is a dotted quad,
//     parsed here; any other name goes to the resolver (5 s, 2 retries), unless AI_NUMERICHOST: then EAI_NONAME.
//     No host: the loopback address, or INADDR_ANY with AI_PASSIVE. AF_INET6 asked for: EAI_FAMILY.
//   - One result per call: the socket type asked for, SOCK_STREAM when none (code that walks the list then doesn't
//     try the same address twice). AI_CANONNAME gives the name as asked.
//   - A service is a decimal port, or one of the few names in the table below (tcp or udp by the socket type);
//     a name with AI_NUMERICSERV is EAI_NONAME, an unknown one EAI_SERVICE.
//   - getnameinfo is numeric for the host (there is no reverse lookup: NI_NAMEREQD is EAI_NONAME), IPv4 and IPv6;
//     the service is the table's name unless NI_NUMERICSERV, else the number. Too small a buffer: EAI_OVERFLOW.
// The resolver's functions are declared here (they are not in the SDK's headers); the host tests link fakes of them.

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *hostname, std::uint32_t *address, int timeout_us, int retries,
                            int flags);
int sceNetResolverDestroy(int resolver);
}

namespace {

constexpr int kResolverTimeoutUs = 5 * 1000 * 1000;
constexpr int kResolverRetries = 2;
constexpr int kPoolSize = 16 * 1024;

std::uint16_t to_net16(std::uint16_t v)
{
    const unsigned char b[2] = {static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)};
    std::uint16_t out;
    std::memcpy(&out, b, 2);
    return out;
}

std::uint16_t from_net16(std::uint16_t v)
{
    unsigned char b[2];
    std::memcpy(b, &v, 2);
    return static_cast<std::uint16_t>((b[0] << 8) | b[1]);
}

std::uint32_t net_address(unsigned a, unsigned b, unsigned c, unsigned d)
{
    const unsigned char bytes[4] = {static_cast<unsigned char>(a), static_cast<unsigned char>(b),
                                    static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
    std::uint32_t out;
    std::memcpy(&out, bytes, 4);
    return out;
}

// a dotted quad: four decimal numbers 0..255, nothing else (as inet_pton for AF_INET)
bool parse_ipv4(const char *text, std::uint32_t &out)
{
    unsigned part[4];
    const char *p = text;
    for (int i = 0; i < 4; ++i)
    {
        if (*p < '0' || *p > '9')
            return false;
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9')
        {
            v = v * 10 + static_cast<unsigned>(*p++ - '0');
            if (++digits > 3 || v > 255)
                return false;
        }
        if (digits > 1 && p[-digits] == '0')
            return false;   // no leading zeros (they would read as octal elsewhere)
        part[i] = v;
        if (i < 3 && *p++ != '.')
            return false;
    }
    if (*p != '\0')
        return false;
    out = net_address(part[0], part[1], part[2], part[3]);
    return true;
}

struct Service
{
    const char *name;
    int port;
    bool tcp, udp;
};

// well-known ports (IANA) of the protocols the ports' network code speaks, and a few common ones
const Service kServices[] = {
    {"ftp", 21, true, false},          {"ssh", 22, true, true},       {"telnet", 23, true, false},
    {"domain", 53, true, true},        {"http", 80, true, true},      {"sunrpc", 111, true, true},
    {"rpcbind", 111, true, true},      {"netbios-ns", 137, true, true}, {"netbios-ssn", 139, true, false},
    {"https", 443, true, true},        {"microsoft-ds", 445, true, true}, {"nfs", 2049, true, true},
};

const Service *find_service(const char *name, bool udp)
{
    for (const Service &s : kServices)
        if (std::strcmp(s.name, name) == 0 && (udp ? s.udp : s.tcp))
            return &s;
    return nullptr;
}

const Service *find_port(int port, bool udp)
{
    for (const Service &s : kServices)
        if (s.port == port && (udp ? s.udp : s.tcp))
            return &s;
    return nullptr;
}

// a decimal port 0..65535; false when it isn't one
bool parse_port(const char *text, int &port)
{
    if (*text == '\0')
        return false;
    long v = 0;
    for (const char *p = text; *p; ++p)
    {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10 + (*p - '0');
        if (v > 65535)
            return false;
    }
    port = static_cast<int>(v);
    return true;
}

std::mutex g_pool_lock;
int g_pool = -1;

int resolver_pool()
{
    std::lock_guard<std::mutex> lock(g_pool_lock);
    if (g_pool < 0)
    {
        sceNetInit();   // the app may have done it already: then this one fails, harmlessly
        g_pool = sceNetPoolCreate("ps5rt-netdb", kPoolSize, 0);
    }
    return g_pool;
}

// 0, or an EAI_ code
int resolve(const char *name, std::uint32_t &out)
{
    const int pool = resolver_pool();
    if (pool < 0)
        return EAI_FAIL;
    const int resolver = sceNetResolverCreate("ps5rt-netdb", pool, 0);
    if (resolver < 0)
        return EAI_FAIL;
    std::uint32_t address = 0;
    const int rc = sceNetResolverStartNtoa(resolver, name, &address, kResolverTimeoutUs, kResolverRetries, 0);
    sceNetResolverDestroy(resolver);
    if (rc < 0 || address == 0)
        return EAI_NONAME;
    out = address;
    return 0;
}

struct Result
{
    addrinfo info;   // first: freeaddrinfo frees the block through it
    sockaddr_in address;
};

bool format_ipv6(const unsigned char *a, char *out, std::size_t size)
{
    unsigned short w[8];
    for (int i = 0; i < 8; ++i)
        w[i] = static_cast<unsigned short>((a[2 * i] << 8) | a[2 * i + 1]);
    // the longest run of two or more zero groups becomes "::" (RFC 5952)
    int best = -1, best_len = 0;
    for (int i = 0; i < 8;)
    {
        if (w[i] != 0)
        {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && w[j] == 0)
            ++j;
        if (j - i > best_len && j - i >= 2)
        {
            best = i;
            best_len = j - i;
        }
        i = j;
    }
    char text[48];
    std::size_t n = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (i == best)
        {
            n += static_cast<std::size_t>(std::snprintf(text + n, sizeof text - n, "::"));
            i += best_len - 1;
            continue;
        }
        n += static_cast<std::size_t>(
            std::snprintf(text + n, sizeof text - n, "%x%s", w[i], i < 7 && i + 1 != best ? ":" : ""));
    }
    if (n + 1 > size)
        return false;
    std::memcpy(out, text, n + 1);
    return true;
}

} // namespace

extern "C" int getaddrinfo(const char *node, const char *service, const addrinfo *hints, addrinfo **res)
{
    if (res == nullptr)
        return EAI_FAIL;
    *res = nullptr;
    const int flags = hints ? hints->ai_flags : 0;
    const int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    constexpr int kKnownFlags = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV
#ifdef AI_ADDRCONFIG
                                | AI_ADDRCONFIG
#endif
#ifdef AI_V4MAPPED
                                | AI_V4MAPPED
#endif
#ifdef AI_ALL
                                | AI_ALL
#endif
        ;
    if (flags & ~kKnownFlags)
        return EAI_BADFLAGS;
    if ((node == nullptr || *node == '\0') && (service == nullptr || *service == '\0'))
        return EAI_NONAME;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;
    if (socktype == 0)
        socktype = protocol == IPPROTO_UDP ? SOCK_DGRAM : SOCK_STREAM;
    if (socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;
    if (protocol == 0)
        protocol = socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    else if (protocol != (socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP))
        return EAI_SOCKTYPE;

    int port = 0;
    if (service != nullptr && *service != '\0' && !parse_port(service, port))
    {
        if (flags & AI_NUMERICSERV)
            return EAI_NONAME;
        const Service *s = find_service(service, socktype == SOCK_DGRAM);
        if (s == nullptr)
            return EAI_SERVICE;
        port = s->port;
    }

    std::uint32_t address = 0;
    if (node == nullptr || *node == '\0')
        address = (flags & AI_PASSIVE) ? net_address(0, 0, 0, 0) : net_address(127, 0, 0, 1);
    else if (!parse_ipv4(node, address))
    {
        if (flags & AI_NUMERICHOST)
            return EAI_NONAME;
        if (const int rc = resolve(node, address))
            return rc;
    }

    Result *r = static_cast<Result *>(std::calloc(1, sizeof(Result)));
    if (r == nullptr)
        return EAI_MEMORY;
#ifdef __FreeBSD__
    r->address.sin_len = sizeof r->address;
#endif
    r->address.sin_family = AF_INET;
    r->address.sin_port = to_net16(static_cast<std::uint16_t>(port));
    r->address.sin_addr.s_addr = address;
    r->info.ai_family = AF_INET;
    r->info.ai_socktype = socktype;
    r->info.ai_protocol = protocol;
    r->info.ai_addrlen = sizeof r->address;
    r->info.ai_addr = reinterpret_cast<sockaddr *>(&r->address);
    if ((flags & AI_CANONNAME) && node != nullptr && *node != '\0')
    {
        const std::size_t n = std::strlen(node) + 1;
        r->info.ai_canonname = static_cast<char *>(std::malloc(n));
        if (r->info.ai_canonname == nullptr)
        {
            std::free(r);
            return EAI_MEMORY;
        }
        std::memcpy(r->info.ai_canonname, node, n);
    }
    *res = &r->info;
    return 0;
}

extern "C" void freeaddrinfo(addrinfo *ai)
{
    while (ai != nullptr)
    {
        addrinfo *next = ai->ai_next;
        std::free(ai->ai_canonname);
        std::free(ai);
        ai = next;
    }
}

#ifdef __FreeBSD__
using NameLength = std::size_t;   // FreeBSD's prototype
#else
using NameLength = socklen_t;     // POSIX / glibc
#endif

extern "C" int getnameinfo(const sockaddr *sa, socklen_t salen, char *host, NameLength hostlen, char *serv,
                           NameLength servlen, int flags)
{
    if (sa == nullptr)
        return EAI_FAIL;
    const bool want_host = host != nullptr && hostlen > 0;
    const bool want_serv = serv != nullptr && servlen > 0;
    if (!want_host && !want_serv)
        return EAI_NONAME;
    int port;
    if (sa->sa_family == AF_INET)
    {
        if (salen < static_cast<socklen_t>(sizeof(sockaddr_in)))
            return EAI_FAIL;
        const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(sa);
        port = from_net16(in->sin_port);
        if (want_host)
        {
            if (flags & NI_NAMEREQD)
                return EAI_NONAME;
            unsigned char b[4];
            std::memcpy(b, &in->sin_addr.s_addr, 4);
            const int n = std::snprintf(host, hostlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (n < 0 || static_cast<std::size_t>(n) >= hostlen)
                return EAI_OVERFLOW;
        }
    }
    else if (sa->sa_family == AF_INET6)
    {
        if (salen < static_cast<socklen_t>(sizeof(sockaddr_in6)))
            return EAI_FAIL;
        const sockaddr_in6 *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
        port = from_net16(in6->sin6_port);
        if (want_host)
        {
            if (flags & NI_NAMEREQD)
                return EAI_NONAME;
            if (!format_ipv6(reinterpret_cast<const unsigned char *>(&in6->sin6_addr), host, hostlen))
                return EAI_OVERFLOW;
        }
    }
    else
        return EAI_FAMILY;
    if (want_serv)
    {
        const Service *s = (flags & NI_NUMERICSERV) ? nullptr : find_port(port, (flags & NI_DGRAM) != 0);
        const int n = s ? std::snprintf(serv, servlen, "%s", s->name) : std::snprintf(serv, servlen, "%d", port);
        if (n < 0 || static_cast<std::size_t>(n) >= servlen)
            return EAI_OVERFLOW;
    }
    return 0;
}

extern "C" const char *gai_strerror(int code)
{
    switch (code)
    {
    case 0:
        return "Success";
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY:
        return "Address family for hostname not supported";
#endif
    case EAI_AGAIN:
        return "Temporary failure in name resolution";
    case EAI_BADFLAGS:
        return "Invalid value for ai_flags";
    case EAI_FAIL:
        return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:
        return "ai_family not supported";
    case EAI_MEMORY:
        return "Memory allocation failure";
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA:
        return "No address associated with hostname";
#endif
    case EAI_NONAME:
        return "hostname nor servname provided, or not known";
    case EAI_SERVICE:
        return "servname not supported for ai_socktype";
    case EAI_SOCKTYPE:
        return "ai_socktype not supported";
    case EAI_SYSTEM:
        return "System error returned in errno";
#ifdef EAI_BADHINTS
    case EAI_BADHINTS:
        return "Invalid value for hints";
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL:
        return "Resolved protocol is unknown";
#endif
    case EAI_OVERFLOW:
        return "Argument buffer overflow";
    default:
        return "Unknown error";
    }
}

namespace {
char kTcp[] = "tcp", kUdp[] = "udp", kIcmp[] = "icmp";
char *kNoAliases[] = {nullptr};
protoent kProtocols[] = {{kTcp, kNoAliases, IPPROTO_TCP}, {kUdp, kNoAliases, IPPROTO_UDP},
                         {kIcmp, kNoAliases, IPPROTO_ICMP}};
servent g_servent;
char g_serv_name[16];
char g_serv_proto[4];

servent *fill_servent(const Service *s, bool udp)
{
    if (s == nullptr)
        return nullptr;
    std::snprintf(g_serv_name, sizeof g_serv_name, "%s", s->name);
    std::snprintf(g_serv_proto, sizeof g_serv_proto, "%s", udp ? "udp" : "tcp");
    g_servent.s_name = g_serv_name;
    g_servent.s_aliases = kNoAliases;
    g_servent.s_port = to_net16(static_cast<std::uint16_t>(s->port));
    g_servent.s_proto = g_serv_proto;
    return &g_servent;
}
} // namespace

extern "C" protoent *getprotobyname(const char *name)
{
    for (protoent &p : kProtocols)
        if (name != nullptr && std::strcmp(p.p_name, name) == 0)
            return &p;
    return nullptr;
}

extern "C" protoent *getprotobynumber(int proto)
{
    for (protoent &p : kProtocols)
        if (p.p_proto == proto)
            return &p;
    return nullptr;
}

extern "C" servent *getservbyname(const char *name, const char *proto)
{
    if (name == nullptr)
        return nullptr;
    const bool udp = proto != nullptr && std::strcmp(proto, "udp") == 0;
    if (proto != nullptr && !udp && std::strcmp(proto, "tcp") != 0)
        return nullptr;
    return fill_servent(find_service(name, udp), udp);
}

extern "C" servent *getservbyport(int port, const char *proto)
{
    const bool udp = proto != nullptr && std::strcmp(proto, "udp") == 0;
    if (proto != nullptr && !udp && std::strcmp(proto, "tcp") != 0)
        return nullptr;
    return fill_servent(find_port(from_net16(static_cast<std::uint16_t>(port)), udp), udp);
}
