#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sockets.h"

static void require(int condition, const char *operation)
{
    if (!condition) {
        perror(operation);
        exit(1);
    }
}

static int check(const char *name, int sock, rfbBool expected, int expected_error)
{
    rfbBool actual;
    int saved_error, passed;
    require(sock >= 0 && sock < FD_SETSIZE, "descriptor range");
    errno = EINPROGRESS;
    actual = sock_wait_for_connected(sock, 1);
    saved_error = errno;
    passed = actual == expected && (expected || saved_error == expected_error);
    printf("%s: %s result=%d errno=%d\n", name,
           passed ? "PASS" : "FAIL", actual, saved_error);
    return !passed;
}

static int connect_to(struct sockaddr_in *address, int require_pending)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int flags, result;
    require(sock >= 0, "socket");
    flags = fcntl(sock, F_GETFL);
    require(flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0,
            "nonblocking");
    result = connect(sock, (struct sockaddr *)address, sizeof(*address));
    require((result < 0 && errno == EINPROGRESS) ||
            (!require_pending && result == 0), "connect must enter helper");
    if (result < 0)
        puts("entered helper after EINPROGRESS");
    return sock;
}

int main(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int reserved, sock, accepted, pipefd[2], failures;
    reserved = socket(AF_INET, SOCK_STREAM, 0);
    require(reserved >= 0, "reserved socket");
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(reserved, (struct sockaddr *)&address, sizeof(address)) == 0,
            "bind");
    require(getsockname(reserved, (struct sockaddr *)&address, &length) == 0,
            "getsockname");
    close(reserved);

    sock = connect_to(&address, 1);
    failures = check("native refused connection", sock, FALSE, ECONNREFUSED);
    close(sock);

    reserved = socket(AF_INET, SOCK_STREAM, 0);
    require(reserved >= 0, "listener socket");
    address.sin_port = 0;
    require(bind(reserved, (struct sockaddr *)&address, sizeof(address)) == 0 &&
            getsockname(reserved, (struct sockaddr *)&address, &length) == 0 &&
            listen(reserved, 1) == 0, "listener");
    sock = connect_to(&address, 0);
    failures += check("native successful connection", sock, TRUE, 0);
    accepted = accept(reserved, NULL, NULL);
    require(accepted >= 0, "accept");
    close(accepted);
    close(sock);
    close(reserved);

    require(pipe(pipefd) == 0, "pipe");
    failures += check("native failed SO_ERROR query", pipefd[1], FALSE, ENOTSOCK);
    close(pipefd[0]);
    close(pipefd[1]);
    return failures != 0;
}
