#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "sockets.h"

static unsigned int interrupted_selects;
static int completion_error;
static volatile sig_atomic_t signals_seen;
static volatile sig_atomic_t slow_handler;

static int observed_select(int nfds, fd_set *readfds, fd_set *writefds,
                           fd_set *exceptfds, struct timeval *timeout)
{
    int result = select(nfds, readfds, writefds, exceptfds, timeout);
    if (result == -1 && errno == EINTR)
        ++interrupted_selects;
    return result;
}

static int observed_getsockopt(int socket, int level, int option,
                               void *value, socklen_t *length)
{
    int result = getsockopt(socket, level, option, value, length);
    if (result == 0 && level == SOL_SOCKET && option == SO_ERROR)
        completion_error = *(int *)value;
    return result;
}

#define select observed_select
#define getsockopt observed_getsockopt
#include "../src/common/sockets.c"
#undef select
#undef getsockopt

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d)\n", \
                __FILE__, __LINE__, #condition, errno); \
        exit(2); \
    } \
} while (0)

static void delay(long nanoseconds)
{
    struct timespec remaining = {nanoseconds / 1000000000, nanoseconds % 1000000000};
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {}
}

static void signal_handler(int signal)
{
    int saved_errno = errno;
    (void)signal;
    ++signals_seen;
    if (slow_handler)
        delay(1100000000);
    errno = saved_errno;
}

static double seconds(void)
{
    struct timespec now;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static int signal_case(int drain, int overrun)
{
    int pair[2], status, error, count = 0;
    char buffer[4096] = {0};
    ssize_t written;
    pid_t child, parent = getpid();
    rfbBool result;
    double start, elapsed;
    unsigned int interruptions;
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    REQUIRE(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    REQUIRE(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
    while ((written = send(pair[0], buffer, sizeof(buffer), 0)) > 0)
        count += written;
    REQUIRE(written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    REQUIRE(count > 0);
    interrupted_selects = 0;
    signals_seen = 0;
    slow_handler = overrun;
    child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        int i, train = overrun ? 1 : drain ? 20 : 80;
        close(pair[0]);
        for (i = 0; i < train; ++i) {
            delay(20000000);
            if (kill(parent, SIGUSR1) != 0)
                _exit(3);
        }
        if (drain) {
            while (recv(pair[1], buffer, sizeof(buffer), 0) > 0) {}
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                _exit(4);
        }
        delay(100000000);
        close(pair[1]);
        _exit(0);
    }
    start = seconds();
    result = sock_wait_for_connected(pair[0], drain ? 5 : 1);
    error = errno;
    elapsed = seconds() - start;
    interruptions = interrupted_selects;
    while (waitpid(child, &status, 0) == -1)
        REQUIRE(errno == EINTR);
    close(pair[0]);
    close(pair[1]);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("signals drain=%d overrun=%d result=%d errno=%d elapsed=%.3f signals=%d interrupted_selects=%u\n",
           drain, overrun, result, error, elapsed, signals_seen, interruptions);
    if (!interruptions || !signals_seen)
        return 1;
    if (drain)
        return !result || elapsed < 0.30 || elapsed > 4.50;
    return result || error != ETIMEDOUT || elapsed < 0.99 || elapsed > 1.50;
}

static int tcp_case(int listening)
{
    struct sockaddr_in address = {0};
    socklen_t length = sizeof(address);
    int destination, sender, connected, error;
    rfbBool result;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination = socket(AF_INET, SOCK_STREAM, 0);
    sender = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(destination >= 0 && sender >= 0);
    REQUIRE(bind(destination, (struct sockaddr *)&address, sizeof(address)) == 0);
    REQUIRE(bind(sender, (struct sockaddr *)&address, sizeof(address)) == 0);
    REQUIRE(getsockname(destination, (struct sockaddr *)&address, &length) == 0);
    if (listening) {
        REQUIRE(listen(destination, 1) == 0);
    } else {
        close(destination);
        destination = -1;
    }
    REQUIRE(fcntl(sender, F_SETFL, O_NONBLOCK) == 0);
    connected = connect(sender, (struct sockaddr *)&address, sizeof(address));
    error = errno;
    printf("tcp listening=%d connect=%d errno=%d EINPROGRESS=%d\n",
           listening, connected, error, EINPROGRESS);
    REQUIRE(connected == -1 && error == EINPROGRESS);
    completion_error = -1;
    result = sock_wait_for_connected(sender, 1);
    if (listening && result) {
        char value;
        int peer = accept(destination, NULL, NULL);
        REQUIRE(peer >= 0);
        REQUIRE(send(sender, "V", 1, 0) == 1);
        REQUIRE(recv(peer, &value, 1, 0) == 1 && value == 'V');
        close(peer);
    }
    close(sender);
    if (destination >= 0)
        close(destination);
    printf("tcp listening=%d helper_result=%d SO_ERROR=%d ECONNREFUSED=%d\n",
           listening, result, completion_error, ECONNREFUSED);
    return !!result != !!listening ||
           completion_error != (listening ? 0 : ECONNREFUSED);
}

int main(void)
{
    struct sigaction action = {0};
    int pair[2], closed_socket, failures = 0;
    action.sa_handler = signal_handler;
    REQUIRE(sigemptyset(&action.sa_mask) == 0);
    REQUIRE(sigaction(SIGUSR1, &action, NULL) == 0);
    failures += signal_case(1, 0);
    failures += signal_case(0, 0);
    failures += signal_case(0, 1);
    slow_handler = 0;
    failures += tcp_case(1);
    failures += tcp_case(0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    REQUIRE(sock_wait_for_connected(pair[0], 0));
    closed_socket = pair[0];
    close(pair[0]);
    REQUIRE(!sock_wait_for_connected(closed_socket, 1) && errno == EBADF);
    close(pair[1]);
    REQUIRE(!sock_wait_for_connected(RFB_INVALID_SOCKET, 1) && errno == EBADF);
    printf("real socket cases: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
