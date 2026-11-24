/* POSIX sockets over host networking. Guest descriptors and sockaddr layouts stay FreeBSD-compatible. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HostSocket;
typedef WSABUF HostBuffer;
#define HOST_SOCKET_INVALID INVALID_SOCKET
#define HOST_POLL WSAPoll
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
typedef int HostSocket;
typedef struct iovec HostBuffer;
#define HOST_SOCKET_INVALID (-1)
#define HOST_POLL poll
#endif

#define FIRST_SOCKET_FD 768
#define MAX_SOCKETS 256
#define GUEST_FD_LIMIT (FIRST_SOCKET_FD + MAX_SOCKETS)
#define GUEST_SOL_SOCKET 0xffffu
#define GUEST_O_NONBLOCK 4

typedef struct { int64_t sec, usec; } GuestTimeval;
typedef struct { uint64_t words[16]; } GuestFdSet;
typedef struct { void *base; uint64_t length; } GuestIovec;
typedef struct {
    void *name;
    uint32_t namelen;
    uint32_t pad0;
    GuestIovec *iov;
    int32_t iovlen;
    uint32_t pad1;
    void *control;
    uint32_t controllen;
    int32_t flags;
} GuestMsghdr;
typedef struct { HostSocket socket; int flags; } SocketEntry;
static SocketEntry sockets[MAX_SOCKETS];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int networking_ready;
static _Thread_local int socket_host_error;

__attribute__((constructor)) static void initialize_socket_table(void) {
    for (int i=0;i<MAX_SOCKETS;++i) sockets[i].socket=HOST_SOCKET_INVALID;
}
static int last_host_error(void) {
#ifdef _WIN32
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return EAGAIN;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEDESTADDRREQ: return EDESTADDRREQ;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEPROTOTYPE: return EPROTOTYPE;
    case WSAENOPROTOOPT: return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT: return ESOCKTNOSUPPORT;
    case WSAEOPNOTSUPP: return EOPNOTSUPP;
    case WSAEPFNOSUPPORT: return EPFNOSUPPORT;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETRESET: return ENETRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAESHUTDOWN: return ESHUTDOWN;
    case WSAETOOMANYREFS: return ETOOMANYREFS;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEHOSTDOWN: return EHOSTDOWN;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAEINVAL: return EINVAL;
    case WSAEFAULT: return EFAULT;
    default: return EIO;
    }
#else
    return errno;
#endif
}
static int64_t fail_socket(void) {
    socket_host_error=last_host_error();
    *runtime_errno()=runtime_guest_errno(socket_host_error);
    return -1;
}
static int64_t fail_errno(int e) {
    socket_host_error=e;
    *runtime_errno()=runtime_guest_errno(e);
    return -1;
}
static int ensure_network(void) {
#ifdef _WIN32
    pthread_mutex_lock(&lock);
    if (!networking_ready) {
        WSADATA data;
        networking_ready=WSAStartup(MAKEWORD(2,2),&data)==0;
    }
    const int ready=networking_ready;
    pthread_mutex_unlock(&lock);
    return ready;
#else
    networking_ready=1;
    return 1;
#endif
}
static int index_of(int fd) { return fd-FIRST_SOCKET_FD; }
static HostSocket get_socket(int fd) {
    const int i=index_of(fd);
    if (i<0 || i>=MAX_SOCKETS) return HOST_SOCKET_INVALID;
    return sockets[i].socket;
}
static int host_family(int family) {
    if (family==2) return AF_INET;
    if (family==28) return AF_INET6;
    return family==1 ? AF_UNSPEC : -1;
}
static int guest_family(int family) { return family==AF_INET6 ? 28 : family; }
static int host_flags(int flags) {
    int result=flags&3;
    if (flags&4) result|=MSG_DONTROUTE;
#ifdef MSG_DONTWAIT
    if (flags&0x80) result|=MSG_DONTWAIT;
#endif
#ifdef MSG_WAITALL
    if (flags&0x100) result|=MSG_WAITALL;
#endif
#ifdef MSG_NOSIGNAL
    if (flags&0x20000) result|=MSG_NOSIGNAL;
#endif
    return result;
}
static int guest_flags(int flags) {
    int result=flags&3;
#ifdef MSG_DONTWAIT
    if (flags&MSG_DONTWAIT) result|=0x80;
#endif
#ifdef MSG_TRUNC
    if (flags&MSG_TRUNC) result|=0x10;
#endif
#ifdef MSG_CTRUNC
    if (flags&MSG_CTRUNC) result|=0x20;
#endif
    return result;
}
static int make_nonblocking(HostSocket socket,int enabled) {
#ifdef _WIN32
    u_long value=enabled != 0;
    return ioctlsocket(socket,FIONBIO,&value) ? last_host_error() : 0;
#else
    const int old=fcntl(socket,F_GETFL,0);
    if (old<0) return errno;
    if (fcntl(socket,F_SETFL,enabled ? old|O_NONBLOCK : old&~O_NONBLOCK)<0) return errno;
    return 0;
#endif
}
static int sockaddr_to_host(const void *guest,uint32_t length,void *host,uint32_t capacity,uint32_t *host_length) {
    const unsigned char *g=guest;
    if (!g || length<4) return EINVAL;
    const uint32_t needed=g[1]==28 ? 28u : g[1]==2 ? 16u : 0;
    if (!needed || length<needed || needed>capacity || (g[0] && g[0]<needed)) return EINVAL;
    memset(host,0,capacity);
    ((unsigned char *)host)[0]=(unsigned char)g[1];
    ((unsigned char *)host)[1]=(unsigned char)(g[1]>>8);
#ifdef _WIN32
    ((struct sockaddr *)host)->sa_family=(ADDRESS_FAMILY)host_family(g[1]);
#else
    ((struct sockaddr *)host)->sa_family=(sa_family_t)host_family(g[1]);
#endif
    if (needed==16) memcpy((unsigned char *)host+2,g+2,14);
    else memcpy((unsigned char *)host+2,g+2,26);
    *host_length=needed;
    return 0;
}
static void sockaddr_to_guest(void *guest,uint32_t *length,const void *host,uint32_t host_length) {
    if (!guest || !length || *length<4) return;
    const uint32_t capacity=*length;
    const struct sockaddr *h=host;
    const uint32_t needed=h->sa_family==AF_INET6 ? 28u : h->sa_family==AF_INET ? 16u : 0;
    if (!needed) { *length=0; return; }
    uint32_t copy=capacity<needed ? capacity : needed;
    if (host_length<copy) copy=host_length;
    unsigned char *g=guest;
    memset(g,0,capacity);
    g[0]=(unsigned char)needed;
    g[1]=(unsigned char)guest_family(h->sa_family);
    const uint32_t bytes=copy>2 ? copy-2 : 0;
    memcpy(g+2,(const unsigned char *)host+2,bytes);
    *length=copy;
}
static ABI int32_t guest_socket(int family,int type,int protocol) {
    if (!ensure_network()) return fail_errno(EAFNOSUPPORT);
    const int af=host_family(family);
    if (af<0) return fail_errno(EAFNOSUPPORT);
    HostSocket value=socket(af,type,protocol);
    if (value==HOST_SOCKET_INVALID) return fail_socket();
    int fd=-1;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_SOCKETS;++i) if (sockets[i].socket==HOST_SOCKET_INVALID) {
        sockets[i]=(SocketEntry){value,0};
        fd=FIRST_SOCKET_FD+i;
        break;
    }
    pthread_mutex_unlock(&lock);
    if (fd<0) {
#ifdef _WIN32
        closesocket(value);
#else
        close(value);
#endif
        return fail_errno(EMFILE);
    }
    return fd;
}
static ABI int32_t guest_bind(int fd,const void *address,uint32_t length) {
    struct sockaddr_storage native;
    uint32_t native_length;
    int e=sockaddr_to_host(address,length,&native,sizeof(native),&native_length);
    if (e) return fail_errno(e);
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    return bind(s,(const struct sockaddr *)&native,(int)native_length) ? fail_socket() : 0;
}
static ABI int32_t guest_connect(int fd,const void *address,uint32_t length) {
    struct sockaddr_storage native;
    uint32_t native_length;
    int e=sockaddr_to_host(address,length,&native,sizeof(native),&native_length);
    if (e) return fail_errno(e);
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    return connect(s,(const struct sockaddr *)&native,(int)native_length) ? fail_socket() : 0;
}
static ABI int32_t guest_listen(int fd,int backlog) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    return listen(s,backlog) ? fail_socket() : 0;
}
static ABI int32_t guest_accept(int fd,void *address,uint32_t *length) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    struct sockaddr_storage native;
#ifdef _WIN32
    int native_length=sizeof(native);
#else
    socklen_t native_length=sizeof(native);
#endif
    HostSocket accepted=accept(s,address ? (struct sockaddr *)&native : NULL,address ? &native_length : NULL);
    if (accepted==HOST_SOCKET_INVALID) return fail_socket();
    int new_fd=-1;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_SOCKETS;++i) if (sockets[i].socket==HOST_SOCKET_INVALID) {
        sockets[i]=(SocketEntry){accepted,0};
        new_fd=FIRST_SOCKET_FD+i;
        break;
    }
    pthread_mutex_unlock(&lock);
    if (new_fd<0) {
#ifdef _WIN32
        closesocket(accepted);
#else
        close(accepted);
#endif
        return fail_errno(EMFILE);
    }
    if (address && length) sockaddr_to_guest(address,length,&native,(uint32_t)native_length);
    return new_fd;
}
static ABI int64_t guest_send(int fd,const void *buffer,uint64_t size,int flags) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (size>INT_MAX) size=INT_MAX;
#ifdef _WIN32
    const int n=send(s,buffer,(int)size,host_flags(flags));
#else
    const ssize_t n=send(s,buffer,(size_t)size,host_flags(flags));
#endif
    return n<0 ? fail_socket() : n;
}
static ABI int64_t guest_recv(int fd,void *buffer,uint64_t size,int flags) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (size>INT_MAX) size=INT_MAX;
#ifdef _WIN32
    const int n=recv(s,buffer,(int)size,host_flags(flags));
#else
    const ssize_t n=recv(s,buffer,(size_t)size,host_flags(flags));
#endif
    return n<0 ? fail_socket() : n;
}
static ABI int64_t guest_sendto(int fd,const void *buffer,uint64_t size,int flags,const void *address,uint32_t length) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    struct sockaddr_storage native;
    uint32_t native_length=0;
    int e=0;
    if (address) e=sockaddr_to_host(address,length,&native,sizeof(native),&native_length);
    if (e) return fail_errno(e);
    if (size>INT_MAX) size=INT_MAX;
#ifdef _WIN32
    const int n=sendto(s,buffer,(int)size,host_flags(flags),address ? (const struct sockaddr *)&native : NULL,(int)native_length);
#else
    const ssize_t n=sendto(s,buffer,(size_t)size,host_flags(flags),address ? (const struct sockaddr *)&native : NULL,native_length);
#endif
    return n<0 ? fail_socket() : n;
}
static ABI int64_t guest_recvfrom(int fd,void *buffer,uint64_t size,int flags,void *address,uint32_t *length) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (address && !length) return fail_errno(EFAULT);
    struct sockaddr_storage native;
#ifdef _WIN32
    int native_length=sizeof(native);
#else
    socklen_t native_length=sizeof(native);
#endif
    if (size>INT_MAX) size=INT_MAX;
#ifdef _WIN32
    const int n=recvfrom(s,buffer,(int)size,host_flags(flags),address ? (struct sockaddr *)&native : NULL,address ? &native_length : NULL);
#else
    const ssize_t n=recvfrom(s,buffer,(size_t)size,host_flags(flags),address ? (struct sockaddr *)&native : NULL,address ? &native_length : NULL);
#endif
    if (n<0) return fail_socket();
    if (address) sockaddr_to_guest(address,length,&native,(uint32_t)native_length);
    return n;
}
static ABI int32_t guest_shutdown(int fd,int how) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    return shutdown(s,how) ? fail_socket() : 0;
}
static int socket_pair(int fd,void *address,uint32_t *length,int peer) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return ENOTSOCK;
    if (!length) return EFAULT;
    struct sockaddr_storage native;
#ifdef _WIN32
    int native_length=sizeof(native);
#else
    socklen_t native_length=sizeof(native);
#endif
    const int result=peer ? getpeername(s,(struct sockaddr *)&native,&native_length)
                          : getsockname(s,(struct sockaddr *)&native,&native_length);
    if (result) return last_host_error();
    if (address) sockaddr_to_guest(address,length,&native,(uint32_t)native_length);
    return 0;
}
static ABI int32_t guest_getsockname(int fd,void *address,uint32_t *length) {
    const int e=socket_pair(fd,address,length,0);
    return e ? fail_errno(e) : 0;
}
static ABI int32_t guest_getpeername(int fd,void *address,uint32_t *length) {
    const int e=socket_pair(fd,address,length,1);
    return e ? fail_errno(e) : 0;
}
static int host_option(int level,int option,int *host_level) {
    if (level!=(int)GUEST_SOL_SOCKET) { *host_level=level; return option; }
    *host_level=SOL_SOCKET;
    switch (option) {
    case 0x0001: return SO_DEBUG;
    case 0x0004: return SO_REUSEADDR;
    case 0x0008: return SO_KEEPALIVE;
    case 0x0020: return SO_BROADCAST;
    case 0x0080: return SO_LINGER;
    case 0x0100: return SO_OOBINLINE;
#ifdef SO_REUSEPORT
    case 0x0200: return SO_REUSEPORT;
#endif
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1003: return SO_SNDLOWAT;
    case 0x1004: return SO_RCVLOWAT;
    case 0x1005: return SO_SNDTIMEO;
    case 0x1006: return SO_RCVTIMEO;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    default: return -1;
    }
}
static ABI int32_t guest_setsockopt(int fd,int level,int option,const void *value,uint32_t length) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (!value && length) return fail_errno(EFAULT);
    int native_level;
    const int native_option=host_option(level,option,&native_level);
    if (native_option<0) return fail_errno(ENOPROTOOPT);
    unsigned char converted[64];
    const void *native_value=value;
    int native_length=(int)length;
    if (native_level==SOL_SOCKET && native_option==SO_LINGER && length==8) {
        const int32_t *source=value;
        if (native_length>(int)sizeof(converted)) return fail_errno(EINVAL);
#ifdef _WIN32
        struct linger target={(u_short)source[0],(u_short)source[1]};
        native_length=sizeof(target);
#else
        struct linger target={source[0],source[1]};
#endif
        memcpy(converted,&target,sizeof(target));
        native_value=converted;
    }
    if ((native_option==SO_SNDTIMEO || native_option==SO_RCVTIMEO) && length==16) {
        const GuestTimeval *source=value;
        if (source->sec<0 || source->usec<0 || source->usec>=1000000) return fail_errno(EDOM);
#ifdef _WIN32
        DWORD milliseconds=(DWORD)(source->sec*1000+source->usec/1000);
        memcpy(converted,&milliseconds,sizeof(milliseconds));
        native_value=converted;
        native_length=sizeof(milliseconds);
#else
        struct timeval target={(time_t)source->sec,(suseconds_t)source->usec};
        memcpy(converted,&target,sizeof(target));
        native_value=converted;
        native_length=sizeof(target);
#endif
    }
    return setsockopt(s,native_level,native_option,native_value,native_length) ? fail_socket() : 0;
}
static ABI int32_t guest_getsockopt(int fd,int level,int option,void *value,uint32_t *length) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (!length || (!value && *length)) return fail_errno(EFAULT);
    int native_level;
    const int native_option=host_option(level,option,&native_level);
    if (native_option<0) return fail_errno(ENOPROTOOPT);
    unsigned char converted[256];
    void *native_value=value;
    uint32_t capacity=*length;
    if ((native_level==SOL_SOCKET && native_option==SO_LINGER && capacity==8) ||
        ((native_option==SO_SNDTIMEO || native_option==SO_RCVTIMEO) && capacity==16)) {
        native_value=converted;
        capacity=sizeof(converted);
    }
#ifdef _WIN32
    int native_length=(int)capacity;
#else
    socklen_t native_length=capacity;
#endif
    if (getsockopt(s,native_level,native_option,native_value,&native_length)) return fail_socket();
    if (native_value==converted && native_level==SOL_SOCKET && native_option==SO_LINGER) {
        const struct linger *source=(const struct linger *)converted;
        int32_t target[2]={source->l_onoff,source->l_linger};
        memcpy(value,target,sizeof(target));
        *length=sizeof(target);
    } else if (native_value==converted && (native_option==SO_SNDTIMEO || native_option==SO_RCVTIMEO)) {
        GuestTimeval target;
#ifdef _WIN32
        const DWORD milliseconds=*(const DWORD *)converted;
        target=(GuestTimeval){milliseconds/1000,(milliseconds%1000)*1000};
#else
        const struct timeval *source=(const struct timeval *)converted;
        target=(GuestTimeval){source->tv_sec,source->tv_usec};
#endif
        memcpy(value,&target,sizeof(target));
        *length=sizeof(target);
    } else *length=(uint32_t)native_length;
    return 0;
}
static void fd_clear(GuestFdSet *set,int fd) { if (set && fd>=0 && fd<GUEST_FD_LIMIT) set->words[fd>>6]&=~(UINT64_C(1)<<(fd&63)); }
static void fd_add(GuestFdSet *set,int fd) { if (set && fd>=0 && fd<GUEST_FD_LIMIT) set->words[fd>>6]|=UINT64_C(1)<<(fd&63); }
static int fd_present(const GuestFdSet *set,int fd) { return set && fd>=0 && fd<GUEST_FD_LIMIT && (set->words[fd>>6]>>(fd&63))&1; }
int runtime_file_is_open(int fd);
/* Sockets are polled; open files and devices are always ready; with nothing to wait on the call is a sleep. */
static ABI int32_t guest_select(int nfds,GuestFdSet *readfds,GuestFdSet *writefds,GuestFdSet *exceptfds,GuestTimeval *timeout) {
    if (nfds<0 || nfds>GUEST_FD_LIMIT || (timeout && (timeout->sec<0 || timeout->usec<0 || timeout->usec>=1000000))) return fail_errno(EINVAL);
    struct pollfd native[GUEST_FD_LIMIT];
    int descriptors[GUEST_FD_LIMIT];
    short requested[GUEST_FD_LIMIT];
    GuestFdSet file_read={{0}}, file_write={{0}};
    int count=0, ready=0;
    for (int fd=0;fd<nfds;++fd) {
        const int want_read=fd_present(readfds,fd), want_write=fd_present(writefds,fd), exceptional=fd_present(exceptfds,fd);
        const short events=(short)((want_read ? POLLIN : 0)|(want_write ? POLLOUT : 0));
        if (!events && !exceptional) continue;
        HostSocket s=count<GUEST_FD_LIMIT ? get_socket(fd) : HOST_SOCKET_INVALID;
        if (s==HOST_SOCKET_INVALID) {
            if (!runtime_file_is_open(fd)) return fail_errno(EBADF);
            if (want_read && fd!=0) { fd_add(&file_read,fd); ++ready; }
            if (want_write) { fd_add(&file_write,fd); ++ready; }
            continue;
        }
        if (!ensure_network()) return fail_errno(EAFNOSUPPORT);
        native[count]=(struct pollfd){s,events,0};
        requested[count]=(short)(events|(exceptional ? 0x4000 : 0));
        descriptors[count++]=fd;
    }
    int milliseconds=-1;
    if (timeout) {
        const int64_t total=timeout->sec*1000+(timeout->usec+999)/1000;
        milliseconds=total>INT_MAX ? INT_MAX : (int)total;
    }
    if (ready) milliseconds=0;
    if (!count) {
        if (!ready) {
            if (milliseconds<0) for (;;) runtime_thread_sleep_ns(UINT64_C(1000000000));
            runtime_thread_sleep_ns((uint64_t)timeout->sec*1000000000u+(uint64_t)timeout->usec*1000u);
        }
    } else {
        const int result=HOST_POLL(native,(uint32_t)count,milliseconds);
        if (result<0) return fail_socket();
    }
    for (int fd=0;fd<nfds;++fd) { fd_clear(readfds,fd); fd_clear(writefds,fd); fd_clear(exceptfds,fd); }
    for (int fd=0;fd<nfds;++fd) {
        if (fd_present(&file_read,fd)) fd_add(readfds,fd);
        if (fd_present(&file_write,fd)) fd_add(writefds,fd);
    }
    for (int i=0;i<count;++i) {
        const short re=native[i].revents;
        const int fd=descriptors[i];
        if ((requested[i]&POLLIN) && (re&(POLLIN|POLLHUP))) { fd_add(readfds,fd); ++ready; }
        if ((requested[i]&POLLOUT) && (re&POLLOUT)) { fd_add(writefds,fd); ++ready; }
        if ((requested[i]&0x4000) && (re&(POLLERR|POLLHUP|POLLNVAL))) { fd_add(exceptfds,fd); ++ready; }
    }
    return ready;
}
static int buffers_from_guest(const GuestMsghdr *message,HostBuffer *out,int *count) {
    if (!message || message->iovlen<0 || message->iovlen>1024 || (message->iovlen && !message->iov)) return EINVAL;
    for (int i=0;i<message->iovlen;++i) {
#ifdef _WIN32
        if (message->iov[i].length>ULONG_MAX) return EMSGSIZE;
        out[i].len=(ULONG)(message->iov[i].length>INT_MAX ? INT_MAX : message->iov[i].length);
        out[i].buf=message->iov[i].base;
#else
        out[i].iov_base=message->iov[i].base;
        out[i].iov_len=message->iov[i].length;
#endif
    }
    *count=message->iovlen;
    return 0;
}
static ABI int64_t guest_sendmsg(int fd,const GuestMsghdr *message,int flags) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (!message || message->controllen) return fail_errno(message ? ENOTSUP : EFAULT);
    HostBuffer buffers[1024];
    int count;
    int e=buffers_from_guest(message,buffers,&count);
    if (e) return fail_errno(e);
    struct sockaddr_storage native;
    uint32_t native_length=0;
    if (message->name) e=sockaddr_to_host(message->name,message->namelen,&native,sizeof(native),&native_length);
    if (e) return fail_errno(e);
#ifdef _WIN32
    DWORD sent=0;
    const int result=WSASendTo(s,buffers,(DWORD)count,&sent,host_flags(flags),message->name ? (const struct sockaddr *)&native : NULL,(int)native_length,NULL,NULL);
    return result ? fail_socket() : (int64_t)sent;
#else
    struct msghdr native_message={0};
    native_message.msg_name=message->name ? &native : NULL;
    native_message.msg_namelen=native_length;
    native_message.msg_iov=buffers;
    native_message.msg_iovlen=(size_t)count;
    const ssize_t sent=sendmsg(s,&native_message,host_flags(flags));
    return sent<0 ? fail_socket() : sent;
#endif
}
static ABI int64_t guest_recvmsg(int fd,GuestMsghdr *message,int flags) {
    HostSocket s=get_socket(fd);
    if (s==HOST_SOCKET_INVALID) return fail_errno(ENOTSOCK);
    if (!message) return fail_errno(EFAULT);
    HostBuffer buffers[1024];
    int count;
    int e=buffers_from_guest(message,buffers,&count);
    if (e) return fail_errno(e);
    struct sockaddr_storage native;
    int native_flags=host_flags(flags);
#ifdef _WIN32
    int native_length=sizeof(native);
    DWORD received=0;
    const int result=WSARecvFrom(s,buffers,(DWORD)count,&received,(LPDWORD)&native_flags,message->name ? (struct sockaddr *)&native : NULL,
                                 message->name ? &native_length : NULL,NULL,NULL);
    if (result) return fail_socket();
#else
    socklen_t native_length=sizeof(native);
    struct msghdr native_message={0};
    native_message.msg_name=message->name ? &native : NULL;
    native_message.msg_namelen=message->name ? native_length : 0;
    native_message.msg_iov=buffers;
    native_message.msg_iovlen=(size_t)count;
    const ssize_t got=recvmsg(s,&native_message,native_flags);
    if (got<0) return fail_socket();
    const int64_t received=got;
    native_flags=native_message.msg_flags;
    native_length=native_message.msg_namelen;
#endif
    if (message->name && message->namelen) sockaddr_to_guest(message->name,&message->namelen,&native,(uint32_t)native_length);
    if (message->control) message->controllen=0;
    message->flags=guest_flags(native_flags);
#ifdef _WIN32
    return (int64_t)received;
#else
    return received;
#endif
}
static ABI int32_t guest_inet_pton(int family,const char *source,void *destination) {
    if (!ensure_network()) return fail_errno(EAFNOSUPPORT);
    const int af=host_family(family);
    if (af<0) return fail_errno(EAFNOSUPPORT);
    if (!source || !destination) return fail_errno(EFAULT);
    const int result=inet_pton(af,source,destination);
    return result<0 ? fail_socket() : result;
}
static ABI const char *guest_inet_ntop(int family,const void *source,char *destination,uint32_t size) {
    if (!ensure_network()) { fail_errno(EAFNOSUPPORT); return NULL; }
    const int af=host_family(family);
    if (af<0) { fail_errno(EAFNOSUPPORT); return NULL; }
    if (!source || !destination) { fail_errno(EFAULT); return NULL; }
#ifdef _WIN32
    if (!inet_ntop(af,source,destination,size)) { fail_socket(); return NULL; }
#else
    if (!inet_ntop(af,source,destination,size)) { fail_socket(); return NULL; }
#endif
    return destination;
}

int64_t runtime_socket_close_file(int fd) {
    const int i=index_of(fd);
    if (i<0 || i>=MAX_SOCKETS || sockets[i].socket==HOST_SOCKET_INVALID) return -EBADF;
    pthread_mutex_lock(&lock);
    HostSocket value=sockets[i].socket;
    sockets[i]=(SocketEntry){HOST_SOCKET_INVALID,0};
    pthread_mutex_unlock(&lock);
#ifdef _WIN32
    return closesocket(value) ? -last_host_error() : 0;
#else
    return close(value) ? -errno : 0;
#endif
}
int64_t runtime_socket_read_file(int fd,void *buffer,uint64_t size) {
    if (get_socket(fd)==HOST_SOCKET_INVALID) return -EBADF;
    const int64_t result=guest_recv(fd,buffer,size,0);
    return result<0 ? -socket_host_error : result;
}
int64_t runtime_socket_write_file(int fd,const void *buffer,uint64_t size) {
    if (get_socket(fd)==HOST_SOCKET_INVALID) return -EBADF;
    const int64_t result=guest_send(fd,buffer,size,0);
    return result<0 ? -socket_host_error : result;
}
int64_t runtime_socket_fcntl_file(int fd,int command,intptr_t argument) {
    const int i=index_of(fd);
    if (i<0 || i>=MAX_SOCKETS || sockets[i].socket==HOST_SOCKET_INVALID) return -EBADF;
    if (command==3) return sockets[i].flags;
    if (command!=4) return -EINVAL;
    const int e=make_nonblocking(sockets[i].socket,(argument&GUEST_O_NONBLOCK)!=0);
    if (e) return -e;
    sockets[i].flags=(sockets[i].flags&3)|((int)argument&~3);
    return 0;
}

static const RuntimeExport exports[]={
    {"socket",guest_socket}, {"bind",guest_bind}, {"connect",guest_connect}, {"listen",guest_listen},
    {"accept",guest_accept}, {"send",guest_send}, {"recv",guest_recv}, {"sendto",guest_sendto},
    {"recvfrom",guest_recvfrom}, {"sendmsg",guest_sendmsg}, {"recvmsg",guest_recvmsg}, {"select",guest_select},
    {"setsockopt",guest_setsockopt}, {"getsockopt",guest_getsockopt}, {"shutdown",guest_shutdown},
    {"getsockname",guest_getsockname}, {"getpeername",guest_getpeername}, {"inet_pton",guest_inet_pton},
    {"inet_ntop",guest_inet_ntop},
};
uintptr_t runtime_posix_socket_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
