#ifndef DDEBUG
#define DDEBUG 0
#endif

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <execinfo.h>
#include <string.h>
#include <search.h>

#if DDEBUG
#   define dd(...) \
        fprintf(stderr, "mockeagain: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, " at %s line %d.\n", __FILE__, __LINE__)
#else
#   define dd(...)
#endif


#define MAX_FD 1024

#define MAX_BACKTRACE 64
#define MAX_WHITELIST 64


static void *libc_handle = NULL;
static short active_fds[MAX_FD + 1];
static char  polled_fds[MAX_FD + 1];
static char  weird_fds[MAX_FD + 1];
static char  snd_timeout_fds[MAX_FD + 1];
static char **matchbufs = NULL;
static size_t matchbuf_len = 0;
static const char *pattern = NULL;
static int verbose = -1;
static int mocking_type = -1;


enum {
    MOCKING_READS = 0x01,
    MOCKING_WRITES = 0x02
};


#   define init_libc_handle() \
        if (libc_handle == NULL) { \
            libc_handle = RTLD_NEXT; \
        }


#define call_original(_symbol, _orig_func, ...)                         \
do {                                                                    \
    init_libc_handle();                                                 \
                                                                        \
    if (_orig_func == NULL) {                                           \
        _orig_func = dlsym(libc_handle, _symbol);                       \
        if (_orig_func == NULL) {                                       \
            fprintf(stderr, "mockeagain: could not find the underlying" \
                    " " _symbol ": %s\n", dlerror());                   \
            exit(1);                                                    \
        }                                                               \
    }                                                                   \
                                                                        \
    if (get_verbose_level()) {                                          \
        fprintf(stderr, "mockeagain: calling the original libc:"        \
                " '" _symbol "'\n");                                    \
                                                                        \
    }                                                                   \
                                                                        \
    retval = (*_orig_func)(__VA_ARGS__);                                \
                                                                        \
 } while (0)



typedef int (*socket_handle) (int domain, int type, int protocol);

typedef int (*poll_handle) (struct pollfd *ufds, unsigned int nfds,
    int timeout);

typedef ssize_t (*writev_handle) (int fildes, const struct iovec *iov,
    int iovcnt);

typedef int (*close_handle) (int fd);

typedef ssize_t (*send_handle) (int sockfd, const void *buf, size_t len,
    int flags);

typedef ssize_t (*read_handle) (int fd, void *buf, size_t count);

typedef ssize_t (*recv_handle) (int sockfd, void *buf, size_t len,
    int flags);

typedef ssize_t (*recvfrom_handle) (int sockfd, void *buf, size_t len,
    int flags, struct sockaddr *src_addr, socklen_t *addrlen);


static int get_verbose_level();
static void init_matchbufs();
static int now();
static int get_mocking_type();
static int is_whitelist();
static int get_whitelist();

#define WHITELIST_UNSET 0x00
#define WHITELIST_ERR   0x01
#define WHITELIST_OK    0x02

static char whitelist_status = WHITELIST_UNSET;

int socket(int domain, int type, int protocol)
{
    int                        fd;
    static socket_handle       orig_socket = NULL;

    dd("calling my socket");

    init_libc_handle();

    if (orig_socket == NULL) {
        orig_socket = dlsym(libc_handle, "socket");
        if (orig_socket == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying socket: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    init_matchbufs();

    fd = (*orig_socket)(domain, type, protocol);

    dd("socket with type %d (SOCK_STREAM %d, SOCK_DGRAM %d)", type,
            SOCK_STREAM, SOCK_DGRAM);

    if (fd <= MAX_FD) {
        if (!(type & SOCK_STREAM)) {
            dd("socket: the current fd is weird: %d", fd);
            weird_fds[fd] = 1;

        } else {
            weird_fds[fd] = 0;
        }

#if 1
        if (matchbufs && matchbufs[fd]) {
            free(matchbufs[fd]);
            matchbufs[fd] = NULL;
        }
#endif

        active_fds[fd] = 0;
        polled_fds[fd] = 0;
        snd_timeout_fds[fd] = 0;
    }

    dd("socket returning %d", fd);

    return fd;
}


int
poll(struct pollfd *ufds, nfds_t nfds, int timeout)
{
    static void             *libc_handle;
    int                      retval;
    static poll_handle       orig_poll = NULL;
    struct pollfd           *p;
    int                      i;
    int                      fd;
    int                      begin = 0;
    int                      elapsed = 0;

    dd("calling my poll");

    init_libc_handle();

    if (orig_poll == NULL) {
        orig_poll = dlsym(libc_handle, "poll");
        if (orig_poll == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying poll: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    init_matchbufs();

    dd("calling the original poll");

    if (pattern) {
        begin = now();
    }

    retval = (*orig_poll)(ufds, nfds, timeout);

    if (pattern) {
        elapsed = now() - begin;
    }

    if (retval > 0) {
        struct timeval  tm;

        p = ufds;
        for (i = 0; i < nfds; i++, p++) {
            fd = p->fd;
            if (fd > MAX_FD || weird_fds[fd]) {
                dd("skipping fd %d", fd);
                continue;
            }

            if (pattern && (p->revents & POLLOUT) && snd_timeout_fds[fd]) {

                if (get_verbose_level()) {
                    fprintf(stderr, "mockeagain: poll: should suppress write "
                            "event on fd %d.\n", fd);
                }

                p->revents &= ~POLLOUT;

                if (p->revents == 0) {
                    retval--;
                    continue;
                }
            }

            active_fds[fd] = p->revents;
            polled_fds[fd] = 1;

            if (get_verbose_level()) {
                fprintf(stderr, "mockeagain: poll: fd %d polled with events "
                        "%d\n", fd, p->revents);
            }
        }

        if (retval == 0) {
            if (get_verbose_level()) {
                fprintf(stderr, "mockeagain: poll: emulating timeout on "
                        "fd %d.\n", fd);
            }

            if (timeout < 0) {
                tm.tv_sec = 3600 * 24;
                tm.tv_usec = 0;

                if (get_verbose_level()) {
                    fprintf(stderr, "mockeagain: poll: sleeping 1 day "
                            "on fd %d.\n", fd);
                }

                select(0, NULL, NULL, NULL, &tm);

            } else {

                if (elapsed < timeout) {
                    int     diff;

                    diff = timeout - elapsed;

                    tm.tv_sec = diff / 1000;
                    tm.tv_usec = diff % 1000 * 1000;

                    if (get_verbose_level()) {
                        fprintf(stderr, "mockeagain: poll: sleeping %d ms "
                                "on fd %d.\n", diff, fd);
                    }

                    select(0, NULL, NULL, NULL, &tm);
                }
            }
        }
    }

    return retval;
}


ssize_t
writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t                  retval;
    static writev_handle     orig_writev = NULL;
    struct iovec             new_iov[1] = { {NULL, 0} };
    const struct iovec      *p;
    int                      i;
    size_t                   len;


    if (is_whitelist()) {
        call_original("writev", orig_writev, fd, iov, iovcnt);
        return retval;
    }

    if ((get_mocking_type() & MOCKING_WRITES)
        && fd <= MAX_FD
        && polled_fds[fd]
        && !(active_fds[fd] & POLLOUT))
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"writev\" on fd %d to "
                    "signal EAGAIN.\n", fd);
        }

        errno = EAGAIN;
        return -1;
    }

    init_libc_handle();

    if (orig_writev == NULL) {
        orig_writev = dlsym(libc_handle, "writev");
        if (orig_writev == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying writev: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    if (!(get_mocking_type() & MOCKING_WRITES)) {
        return (*orig_writev)(fd, iov, iovcnt);
    }

    if (fd <= MAX_FD && polled_fds[fd]) {
        p = iov;
        for (i = 0; i < iovcnt; i++, p++) {
            if (p->iov_base == NULL || p->iov_len == 0) {
                continue;
            }

            new_iov[0].iov_base = p->iov_base;
            new_iov[0].iov_len = 1;
            break;
        }

        len = 0;
        p = iov;
        for (i = 0; i < iovcnt; i++, p++) {
            len += p->iov_len;
        }
    }

    if (new_iov[0].iov_base == NULL) {
        retval = (*orig_writev)(fd, iov, iovcnt);

    } else {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"writev\" on fd %d to emit "
                    "1 of %llu bytes.\n", fd, (unsigned long long) len);
        }

        if (pattern) {
            char          *p;
            size_t         len;
            char           c;

            c = *(char *) new_iov[0].iov_base;

            if (matchbufs[fd] == NULL) {

                matchbufs[fd] = malloc(matchbuf_len);
                if (matchbufs[fd] == NULL) {
                    fprintf(stderr, "mockeagain: ERROR: failed to allocate memory.\n");
                }

                p = matchbufs[fd];
                memset(p, 0, matchbuf_len);

                p[0] = c;

                len = 1;

            } else {
                p = matchbufs[fd];

                len = strlen(p);

                if (len < matchbuf_len - 1) {
                    p[len] = c;
                    len++;

                } else {
                    memmove(p, p + 1, matchbuf_len - 2);

                    p[matchbuf_len - 2] = c;
                }
            }

            /* test if the pattern matches the matchbuf */

            dd("matchbuf: %.*s (len: %d)", (int) len, p,
                    (int) matchbuf_len - 1);

            if (len == matchbuf_len - 1 && strncmp(p, pattern, len) == 0) {
                if (get_verbose_level()) {
                    fprintf(stderr, "mockeagain: \"writev\" has found a match for "
                            "the timeout pattern \"%s\" on fd %d.\n", pattern, fd);
                }

                snd_timeout_fds[fd] = 1;
            }
        }

        dd("calling the original writev on fd %d", fd);
        retval = (*orig_writev)(fd, new_iov, 1);
        active_fds[fd] &= ~POLLOUT;
    }

    return retval;
}


int
close(int fd)
{
    int                     retval;
    static close_handle     orig_close = NULL;

    if (is_whitelist()) {
        call_original("close", orig_close, fd);
        return retval;
    }

    init_libc_handle();

    if (orig_close == NULL) {
        orig_close = dlsym(libc_handle, "close");
        if (orig_close == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying close: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    if (fd <= MAX_FD) {
#if (DDEBUG)
        if (polled_fds[fd]) {
            dd("calling the original close on fd %d", fd);
        }
#endif

        if (matchbufs && matchbufs[fd]) {
            free(matchbufs[fd]);
            matchbufs[fd] = NULL;
        }

        active_fds[fd] = 0;
        polled_fds[fd] = 0;
        snd_timeout_fds[fd] = 0;
        weird_fds[fd] = 0;
    }

    retval = (*orig_close)(fd);

    return retval;
}


ssize_t
send(int fd, const void *buf, size_t len, int flags)
{
    ssize_t                  retval;
    static send_handle       orig_send = NULL;

    dd("calling my send");

    if (is_whitelist()) {
        call_original("send", orig_send, fd, buf, len, flags);
        return retval;
    }

    if ((get_mocking_type() & MOCKING_WRITES)
        && fd <= MAX_FD
        && polled_fds[fd]
        && !(active_fds[fd] & POLLOUT))
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"send\" on fd %d to "
                    "signal EAGAIN\n", fd);
        }

        errno = EAGAIN;
        return -1;
    }

    init_libc_handle();

    if (orig_send == NULL) {
        orig_send = dlsym(libc_handle, "send");
        if (orig_send == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying send: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    if ((get_mocking_type() & MOCKING_WRITES)
        && fd <= MAX_FD
        && polled_fds[fd]
        && len)
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"send\" on fd %d to emit "
                    "1 byte data only\n", fd);
        }

        retval = (*orig_send)(fd, buf, 1, flags);
        active_fds[fd] &= ~POLLOUT;

    } else {

        dd("calling the original send on fd %d", fd);

        retval = (*orig_send)(fd, buf, len, flags);
    }

    return retval;
}


ssize_t
read(int fd, void *buf, size_t len)
{
    ssize_t                  retval;
    static read_handle       orig_read = NULL;

    dd("calling my read");

    if (is_whitelist()) {
        call_original("read", orig_read, fd, buf, len);
        return retval;
    }

    if ((get_mocking_type() & MOCKING_READS)
        && fd <= MAX_FD
        && polled_fds[fd]
        && !(active_fds[fd] & POLLIN))
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"read\" on fd %d to "
                    "signal EAGAIN\n", fd);
        }

        errno = EAGAIN;
        return -1;
    }

    init_libc_handle();

    if (orig_read == NULL) {
        orig_read = dlsym(libc_handle, "read");
        if (orig_read == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying read: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    if ((get_mocking_type() & MOCKING_READS)
        && fd <= MAX_FD
        && polled_fds[fd]
        && len)
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"read\" on fd %d to read "
                    "1 byte only\n", fd);
        }

        dd("calling the original read on fd %d", fd);

        retval = (*orig_read)(fd, buf, 1);
        active_fds[fd] &= ~POLLIN;

    } else {
        retval = (*orig_read)(fd, buf, len);
    }

    return retval;
}


ssize_t
recv(int fd, void *buf, size_t len, int flags)
{
    ssize_t                  retval;
    static recv_handle       orig_recv = NULL;

    dd("calling my recv");

    if (is_whitelist()) {
        call_original("recv", orig_recv, fd, buf, len, flags);
        return retval;
    }

    if ((get_mocking_type() & MOCKING_READS)
        && fd <= MAX_FD
        && polled_fds[fd]
        && !(active_fds[fd] & POLLIN))
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"recv\" on fd %d to "
                    "signal EAGAIN\n", fd);
        }

        errno = EAGAIN;
        return -1;
    }

    init_libc_handle();

    if (orig_recv == NULL) {
        orig_recv = dlsym(libc_handle, "recv");
        if (orig_recv == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying recv: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    if ((get_mocking_type() & MOCKING_READS)
        && fd <= MAX_FD
        && polled_fds[fd]
        && len)
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"recv\" on fd %d to read "
                    "1 byte only\n", fd);
        }

        dd("calling the original recv on fd %d", fd);

        retval = (*orig_recv)(fd, buf, 1, flags);
        active_fds[fd] &= ~POLLIN;

    } else {
        retval = (*orig_recv)(fd, buf, len, flags);
    }

    return retval;
}


ssize_t
recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    ssize_t                  retval;
    static recvfrom_handle   orig_recvfrom = NULL;

    dd("calling my recvfrom");

    if (is_whitelist()) {
        call_original("recvfrom", orig_recvfrom,
                      fd, buf, len, flags, src_addr, addrlen);
        return retval;
    }

    if ((get_mocking_type() & MOCKING_READS)
        && fd <= MAX_FD
        && polled_fds[fd]
        && !(active_fds[fd] & POLLIN))
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"recvfrom\" on fd %d to "
                    "signal EAGAIN\n", fd);
        }

        errno = EAGAIN;
        return -1;
    }

    init_libc_handle();

    if (orig_recvfrom == NULL) {
        orig_recvfrom = dlsym(libc_handle, "recvfrom");
        if (orig_recvfrom == NULL) {
            fprintf(stderr, "mockeagain: could not find the underlying recvfrom: "
                    "%s\n", dlerror());
            exit(1);
        }
    }

    if ((get_mocking_type() & MOCKING_READS)
        && fd <= MAX_FD
        && polled_fds[fd]
        && len)
    {
        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: mocking \"recvfrom\" on fd %d to read "
                    "1 byte only\n", fd);
        }

        dd("calling the original recvfrom on fd %d", fd);

        retval = (*orig_recvfrom)(fd, buf, 1, flags, src_addr, addrlen);
        active_fds[fd] &= ~POLLIN;

    } else {
        retval = (*orig_recvfrom)(fd, buf, len, flags, src_addr, addrlen);
    }

    return retval;
}


static int
get_mocking_type() {
    const char          *p;

#if 1
    if (mocking_type >= 0) {
        return mocking_type;
    }
#endif

    mocking_type = 0;

    p = getenv("MOCKEAGAIN");
    if (p == NULL || *p == '\0') {
        dd("MOCKEAGAIN env empty");
        /* mocking_type = MOCKING_WRITES; */
        return mocking_type;
    }

    while (*p) {
        if (*p == 'r' || *p == 'R') {
            mocking_type |= MOCKING_READS;

        } else if (*p == 'w' || *p == 'W') {
            mocking_type |= MOCKING_WRITES;
        }

        p++;
    }

    if (mocking_type == 0) {
        mocking_type = MOCKING_WRITES;
    }

    dd("mocking_type %d", mocking_type);

    return mocking_type;
}


static int
get_verbose_level()
{
    const char          *p;

    if (verbose >= 0) {
        return verbose;
    }

    p = getenv("MOCKEAGAIN_VERBOSE");
    if (p == NULL || *p == '\0') {
        dd("MOCKEAGAIN_VERBOSE env empty");
        verbose = 0;
        return verbose;
    }

    if (*p >= '0' && *p <= '9') {
        dd("MOCKEAGAIN_VERBOSE env value: %s", p);
        verbose = *p - '0';
        return verbose;
    }

    dd("bad verbose env value: %s", p);
    verbose = 0;
    return verbose;
}


static void
init_matchbufs()
{
    const char          *p;
    int                  len;

    if (matchbufs != NULL) {
        return;
    }

    p = getenv("MOCKEAGAIN_WRITE_TIMEOUT_PATTERN");
    if (p == NULL || *p == '\0') {
        dd("write_timeout env empty");
        matchbuf_len = 0;
        return;
    }

    len = strlen(p);

    matchbufs = malloc((MAX_FD + 1) * sizeof(char *));
    if (matchbufs == NULL) {
        fprintf(stderr, "mockeagain: ERROR: failed to allocate memory.\n");
        return;
    }

    memset(matchbufs, 0, (MAX_FD + 1) * sizeof(char *));
    matchbuf_len = len + 1;

    pattern = p;

    if (get_verbose_level()) {
        fprintf(stderr, "mockeagain: reading write timeout pattern: %s\n",
            pattern);
    }
}


/* returns a time in milliseconds */
static int now() {
   struct timeval tv;

   gettimeofday(&tv, NULL);

   return tv.tv_sec % (3600 * 24) + tv.tv_usec/1000;
}


/* Test if a function is whitelisted in the callstack */
static int is_whitelist()
{
    const char          delimiters[] = "(+";
    void                *buff[MAX_BACKTRACE];
    char                **symbols;
    int                 size;
    int                 i;
    char                *token;
    ENTRY               e;
    ENTRY               *ep = NULL;
    int                 retval = 0;

    if (whitelist_status == WHITELIST_UNSET) {
        dd("initializing whitelist");
        whitelist_status = WHITELIST_ERR;
        get_whitelist();
    }

    if (whitelist_status != WHITELIST_OK) {
        return 0;
    }

    size = backtrace(buff, MAX_BACKTRACE);

    symbols = backtrace_symbols(buff, size);

    for (i = 0; i < size; i++) {

#if DDEBUG
        fprintf(stderr, "\tsymbol: %s\n", symbols[i]);
#endif

        strtok(symbols[i], delimiters);
        token = strtok(NULL, delimiters);

        if (token && (*token == ')' || *token == '0')) {
            /* symbol doesn't contain function name.
             *   e.g : nginx/objs/nginx() [0x43299c],
             *   or : mockeagain/mockeagain.so(+0x3563)
             */
            continue;
        }

#if DDEBUG
        fprintf(stderr, "\tfunction: \"%s\"\n", token);
#endif

        e.key = token;
        ep = hsearch(e, FIND);
        if (ep != NULL) {
            if (get_verbose_level()) {
                fprintf(stderr, "mockeagain: whitelist:"
                        " found function: \"%s\"\n", token);
            }

            retval = 1;
            break;
        }

    }

    free(symbols);
    return retval;
}


/* Get the whitelist from the MOCKEAGAIN_WL env variable */
static int
get_whitelist()
{
    const char          delimiters[] = " ,";
    char                *p;
    char                *token;
    ENTRY               e;
    ENTRY               *ep = NULL;

    p = getenv("MOCKEAGAIN_WL");
    if (p == NULL || *p == '\0') {
        dd("MOCKEAGAIN_WL env empty");
        return 1;
    }

    token = strtok(p, delimiters);

    if (!token) {
        return 1;
    }

    if (hcreate(MAX_WHITELIST) == 0) {
        fprintf(stderr, "mockeagain: whitelist:"
                " Unable to create hash table of"
                " %d elements\n", MAX_WHITELIST);
        return 0;
    }

    whitelist_status = WHITELIST_OK;

    while (token) {

        e.key = token;
        e.data = (void *) 1;

        if (get_verbose_level()) {
            fprintf(stderr, "mockeagain: whitelist:"
                    " adding function \"%s\"\n", token);
        }

        ep = hsearch(e, ENTER);

        if (ep == NULL) {
            fprintf(stderr,
                    "mockeagain: whitelist:"
                    " unable to store entry \"%s\","
                    " MAX_WHITELIST(%d) exceeded.\n",
                    e.key, MAX_WHITELIST);
            return 1;
        }

        token = strtok(NULL, delimiters);
    }

    return 1;
}
