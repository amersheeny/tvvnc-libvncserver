#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "sockets.h"

#ifdef WIN32
#include <windows.h>
#endif

enum scenario {
    READY, INTERRUPTED, DEADLINE, TIMEOUT, FATAL, REFUSED,
    ZERO_READY, ZERO_INTERRUPTED, CLOCK_FAILURE, RETRY_CLOCK_FAILURE
};

static enum scenario current;
static unsigned int calls;
static unsigned int clock_calls;
static unsigned int failures;
static uint64_t now;
static const int test_socket = 7;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s (case %d, call %u)\n", \
                __FILE__, __LINE__, #condition, current, calls); \
        ++failures; \
    } \
} while (0)

static int interrupted_result(void)
{
#ifdef WIN32
    errno = EDOM;
    WSASetLastError(WSAEINTR);
#else
    errno = EINTR;
#endif
    return -1;
}

static int scripted_select(int nfds, fd_set *readfds, fd_set *writefds,
                           fd_set *exceptfds, struct timeval *timeout)
{
    static const unsigned long remaining[] = {1000000, 600000, 600000, 200000};
    unsigned long expected = 1000000;
    ++calls;
    CHECK(nfds == test_socket + 1);
    CHECK(readfds == NULL);
    CHECK(FD_ISSET(test_socket, writefds));
    CHECK(FD_ISSET(test_socket, exceptfds));
    if (current == ZERO_READY || current == ZERO_INTERRUPTED)
        expected = 0;
    else if (current == DEADLINE && calls <= 4)
        expected = remaining[calls - 1];
    else if (current == INTERRUPTED && calls == 2)
        expected = 600000;
    CHECK(timeout->tv_sec * 1000000UL + timeout->tv_usec == expected);
    FD_ZERO(writefds);
    FD_ZERO(exceptfds);
    timeout->tv_sec = 0;
    timeout->tv_usec = 0;
    if (current == TIMEOUT)
        return 0;
    if (current == FATAL || calls > 4) {
#ifdef WIN32
        errno = EDOM;
        WSASetLastError(WSAEINVAL);
#else
        errno = EINVAL;
#endif
        return -1;
    }
    if (current == DEADLINE || current == ZERO_INTERRUPTED ||
        current == RETRY_CLOCK_FAILURE || (current == INTERRUPTED && calls == 1)) {
        if (calls != 2)
            now += 400000;
        return interrupted_result();
    }
    if (current == REFUSED)
        FD_SET(test_socket, exceptfds);
    else
        FD_SET(test_socket, writefds);
    return 1;
}

#ifdef WIN32
static ULONGLONG scripted_ticks(void)
{
    ++clock_calls;
    return now / 1000;
}
#define GetTickCount64 scripted_ticks
#else
static int scripted_clock(clockid_t clock, struct timespec *value)
{
    ++clock_calls;
    CHECK(clock == CLOCK_MONOTONIC);
    if (current == CLOCK_FAILURE ||
        (current == RETRY_CLOCK_FAILURE && clock_calls == 2)) {
        errno = EIO;
        return -1;
    }
    value->tv_sec = now / 1000000;
    value->tv_nsec = (now % 1000000) * 1000;
    return 0;
}

static int scripted_getsockopt(int socket, int level, int option,
                              void *value, socklen_t *length)
{
    CHECK(socket == test_socket);
    CHECK(level == SOL_SOCKET && option == SO_ERROR);
    CHECK(*length == sizeof(int));
    *(int *)value = current == REFUSED ? ECONNREFUSED : 0;
    return 0;
}
#define clock_gettime scripted_clock
#define getsockopt scripted_getsockopt
#endif

#define select scripted_select
#include "../src/common/sockets.c"
#undef select

static void run(enum scenario scenario, unsigned int seconds,
                rfbBool expected, unsigned int expected_calls, int expected_error)
{
    rfbBool result;
    unsigned int before = failures;
    current = scenario;
    calls = clock_calls = 0;
    now = 1000000;
    errno = EDOM;
    result = sock_wait_for_connected(test_socket, seconds);
    CHECK(!!result == !!expected);
    CHECK(calls == expected_calls);
    if (expected_error)
        CHECK(errno == expected_error);
    printf("case=%d result=%d select_calls=%u clock_calls=%u %s\n",
           current, result, calls, clock_calls, before == failures ? "PASS" : "FAIL");
}

int main(void)
{
    run(READY, 1, TRUE, 1, 0);
    run(INTERRUPTED, 1, TRUE, 2, 0);
    run(DEADLINE, 1, FALSE, 4, ETIMEDOUT);
    run(TIMEOUT, 1, FALSE, 1, ETIMEDOUT);
    run(FATAL, 1, FALSE, 1, EINVAL);
    run(REFUSED, 1, FALSE, 1, 0);
    run(ZERO_READY, 0, TRUE, 1, 0);
    run(ZERO_INTERRUPTED, 0, FALSE, 1, ETIMEDOUT);
#ifndef WIN32
    run(CLOCK_FAILURE, 1, FALSE, 0, EIO);
    run(RETRY_CLOCK_FAILURE, 1, FALSE, 1, EIO);
#endif
    calls = clock_calls = 0;
    CHECK(!sock_wait_for_connected(RFB_INVALID_SOCKET, 1));
    CHECK(errno == EBADF);
    CHECK(calls == 0 && clock_calls == 0);
    return failures ? 1 : 0;
}
