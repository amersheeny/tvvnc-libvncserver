#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rfb/rfb.h"

static char error_log[1024];

static void capture_error(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error_log, sizeof(error_log), format, arguments);
    va_end(arguments);
}

int main(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    rfbSocket sock;
    int reserved = socket(AF_INET, SOCK_STREAM, 0);
    int passed;
    if (reserved < 0) {
        perror("socket");
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(reserved, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(reserved, (struct sockaddr *)&address, &length) < 0) {
        perror("reserve loopback port");
        close(reserved);
        return 1;
    }
    close(reserved);
    rfbErr = capture_error;
    sock = rfbConnectToTcpAddr("127.0.0.1", ntohs(address.sin_port));
    passed = sock == RFB_INVALID_SOCKET &&
             strstr(error_log, strerror(ECONNREFUSED)) != NULL;
    printf("native server refusal log: %s\n%s", passed ? "PASS" : "FAIL", error_log);
    if (sock != RFB_INVALID_SOCKET)
        close(sock);
    return !passed;
}
