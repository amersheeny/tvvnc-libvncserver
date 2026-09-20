/* TV Console additions, 2026-09-21. GPL-2.0-or-later, as LibVNCClient.
 * Tests the actual socket reader; CHECK remains active in release builds. */
#include <rfb/rfbclient.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static uint64_t millis(void) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
struct writer { int fd; unsigned size, pieces, gap; };
static void* send_fragments(void* raw) {
  struct writer* w = raw;
  unsigned i, offset = 0, part = w->size / w->pieces;
  unsigned char* data = malloc(w->size);
  CHECK(data != NULL);
  for (i = 0; i < w->size; ++i) data[i] = (unsigned char)(i % 251);
  for (i = 0; i < w->pieces; ++i) {
    unsigned count = i + 1 == w->pieces ? w->size - offset : part;
    if (i) usleep(w->gap);
    while (count) {
      ssize_t sent = write(w->fd, data + offset, count);
      if (sent <= 0) { free(data); return NULL; }
      offset += sent; count -= sent;
    }
  }
  free(data); return NULL;
}
static rfbClient* client_pair(int fd[2]) {
  rfbClient* client;
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  CHECK(fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK) == 0);
  client = rfbGetClient(8, 3, 4);
  CHECK(client != NULL);
  client->sock = fd[0]; client->readTimeout = 1;
  return client;
}
static void fragmented(unsigned size, unsigned pieces, unsigned gap, unsigned timeout, rfbBool expected) {
  int fd[2]; unsigned i;
  pthread_t thread;
  rfbClient* client = client_pair(fd);
  struct writer writer = {fd[1], size, pieces, gap};
  char* result = malloc(size);
  uint64_t started, elapsed;
  rfbBool accepted;
  CHECK(result != NULL);
  client->readTimeout = timeout;
  CHECK(pthread_create(&thread, NULL, send_fragments, &writer) == 0);
  started = millis();
  accepted = ReadFromRFBServer(client, result, size);
  elapsed = millis() - started;
  shutdown(fd[0], SHUT_RDWR); /* Also stops the writer after expected timeout. */
  CHECK(pthread_join(thread, NULL) == 0);
  printf("fragmented bytes=%u pieces=%u accepted=%d elapsed_ms=%llu\n", size, pieces, accepted ? 1 : 0, (unsigned long long)elapsed);
  CHECK(accepted == expected);
  if (accepted) for (i = 0; i < size; ++i) CHECK((unsigned char)result[i] == i % 251);
  if (expected && timeout) CHECK(elapsed < 1000);
  if (!expected) CHECK(elapsed >= 900 && elapsed < 2500);
  free(result); rfbClientCleanup(client); close(fd[1]);
}
int main(void) {
  int fd[2]; char byte;
  uint64_t started;
  rfbClient* client;
  signal(SIGPIPE, SIG_IGN);
  fragmented(2, 2, 50000, 1, TRUE);
  fragmented(20, 20, 20000, 1, TRUE);
  fragmented(307200, 20, 20000, 1, TRUE);
  fragmented(80, 80, 20000, 1, FALSE);
  fragmented(2, 2, 1100000, 0, TRUE);

  client = client_pair(fd);
  client->buf[0] = 'A'; client->buffered = 1; client->bufoutptr = client->buf;
  started = millis();
  CHECK(WaitForMessage(client, 1000000) == 1 && millis() - started < 250);
  CHECK(ReadFromRFBServer(client, &byte, 1) && byte == 'A');
  started = millis();
  CHECK(!ReadFromRFBServer(client, &byte, 1));
  CHECK(millis() - started >= 900 && millis() - started < 2500);
  shutdown(fd[1], SHUT_WR);
  CHECK(!ReadFromRFBServer(client, &byte, 1));
  rfbClientCleanup(client); close(fd[1]);

  client = rfbGetClient(8, 3, 4);
  client->sock = RFB_INVALID_SOCKET;
  CHECK(WaitForMessage(client, 1000) == -1);
  client->serverPort = -1;
  CHECK(WaitForMessage(client, 1000000) == 1);
  client->serverPort = 5900;
  rfbClientCleanup(client);
  puts("PASS buffered, fragmented, cumulative idle, unlimited, EOF, invalid fd and replay reads");
  return 0;
}
