#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sockets.h"

static int test_socket, query_result, query_error, query_value, query_calls;

static int query(int sock, int level, int option, void *value, socklen_t *length)
{
    if (sock != test_socket || level != SOL_SOCKET || option != SO_ERROR ||
        *length != sizeof(int))
        abort();
    ++query_calls;
    *(int *)value = query_value;
    if (query_result < 0)
        errno = query_error;
    return query_result;
}

#define getsockopt query
#include "../src/common/sockets.c"
#undef getsockopt

static int run(const char *name, int result, int value, int error,
               rfbBool expected, int expected_error)
{
    rfbBool actual;
    int saved_error, passed;
    query_result = result;
    query_value = value;
    query_error = error;
    query_calls = 0;
    errno = EINPROGRESS;
    actual = sock_wait_for_connected(test_socket, 1);
    saved_error = errno;
    passed = query_calls == 1 && actual == expected &&
             (expected || saved_error == expected_error);
    printf("%s: %s result=%d errno=%d queries=%d\n",
           name, passed ? "PASS" : "FAIL", actual, saved_error, query_calls);
    return !passed;
}

int main(void)
{
    int pair[2], failures;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        perror("socketpair");
        return 1;
    }
    test_socket = pair[0];
    if (test_socket >= FD_SETSIZE)
        return 1;
    failures = run("query failure with zero output", -1, 0, ENOBUFS,
                   FALSE, ENOBUFS);
    failures += run("query failure with nonzero output", -1, ECONNREFUSED,
                    ENOBUFS, FALSE, ENOBUFS);
    failures += run("SO_ERROR refusal", 0, ECONNREFUSED, 0,
                    FALSE, ECONNREFUSED);
    failures += run("SO_ERROR success", 0, 0, 0, TRUE, 0);
    close(pair[0]);
    close(pair[1]);
    return failures != 0;
}
