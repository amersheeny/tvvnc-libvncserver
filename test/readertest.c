/* TV VNC additions, 2026-09-21 and 2026-09-23. GPL-2.0-or-later, as LibVNCClient.
 * Tests the actual socket reader; CHECK remains active in release builds. */
#include <rfb/rfbclient.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

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
static volatile sig_atomic_t reading, signals_caught;
static void interrupt_handler(int value) {
  (void)value;
  if (reading) ++signals_caught;
}
struct interrupter { pthread_t reader; int fd; unsigned size, count; };
static void* interrupt_reader(void* raw) {
  struct interrupter* sender = raw;
  unsigned i;
  for (i = 0; i < sender->count; ++i) {
    usleep(20000);
    CHECK(pthread_kill(sender->reader, SIGUSR1) == 0);
  }
  if (sender->size) {
    struct writer writer = {sender->fd, sender->size, 1, 0};
    send_fragments(&writer);
  }
  return NULL;
}
static void interrupted(unsigned size, rfbBool send_data, rfbBool message_wait) {
  int fd[2], accepted, caught;
  unsigned i;
  uint64_t started, elapsed;
  pthread_t thread;
  rfbClient* client = client_pair(fd);
  char* bytes = malloc(size);
  struct sigaction handler = {0}, previous;
  struct interrupter sender = {pthread_self(), fd[1], send_data ? size : 0,
      send_data || message_wait ? 4 : 80};
  CHECK(bytes != NULL);
  handler.sa_handler = interrupt_handler;
  CHECK(sigemptyset(&handler.sa_mask) == 0);
  CHECK(sigaction(SIGUSR1, &handler, &previous) == 0);
  signals_caught = 0;
  CHECK(pthread_create(&thread, NULL, interrupt_reader, &sender) == 0);
  started = millis();
  reading = 1;
  accepted = message_wait ? WaitForMessage(client, 500000) : ReadFromRFBServer(client, bytes, size);
  reading = 0;
  caught = signals_caught;
  elapsed = millis() - started;
  shutdown(fd[0], SHUT_RDWR);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(sigaction(SIGUSR1, &previous, NULL) == 0);
  printf("interrupted bytes=%u send=%d message=%d result=%d signals=%d elapsed_ms=%llu\n",
      size, send_data, message_wait, accepted, caught, (unsigned long long)elapsed);
  CHECK(caught > 0);
  CHECK(message_wait ? accepted == 0 : (accepted != 0) == (send_data != 0));
  if (message_wait) CHECK(elapsed < 250);
  else if (!send_data) CHECK(elapsed >= 900 && elapsed < 2500);
  else for (i = 0; i < size; ++i) CHECK((unsigned char)bytes[i] == i % 251);
  free(bytes); rfbClientCleanup(client); close(fd[1]);
}
struct drain { struct interrupter sender; unsigned filled, received; rfbBool valid; };
static void* drain_interrupted_write(void* raw) {
  struct drain* sink = raw;
  char bytes[4096];
  ssize_t count;
  interrupt_reader(&sink->sender);
  while ((count = read(sink->sender.fd, bytes, sizeof(bytes))) > 0) {
    unsigned i;
    for (i = 0; i < (unsigned)count; ++i) {
      unsigned offset = sink->received + i;
      char expected = offset < sink->filled ? 0 : offset == sink->filled ? 'V' : 'N';
      if (offset >= sink->filled + 2 || bytes[i] != expected) sink->valid = FALSE;
    }
    sink->received += (unsigned)count;
    if (sink->received >= sink->filled + 2) break;
  }
  return NULL;
}
static void interrupted_write(void) {
  int fd[2], limit = 4096, caught;
  char fill[4096] = {0};
  ssize_t count;
  pthread_t thread;
  rfbBool accepted;
  struct sigaction handler = {0}, previous;
  rfbClient* client = client_pair(fd);
  struct drain sink = {{pthread_self(), fd[1], 0, 4}, 0, 0, TRUE};
  CHECK(setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &limit, sizeof(limit)) == 0);
  while ((count = write(fd[0], fill, sizeof(fill))) > 0) sink.filled += (unsigned)count;
  CHECK(sink.filled > 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  handler.sa_handler = interrupt_handler;
  CHECK(sigemptyset(&handler.sa_mask) == 0);
  CHECK(sigaction(SIGUSR1, &handler, &previous) == 0);
  signals_caught = 0;
  CHECK(pthread_create(&thread, NULL, drain_interrupted_write, &sink) == 0);
  reading = 1;
  accepted = WriteToRFBServer(client, "VN", 2);
  reading = 0;
  caught = signals_caught;
  shutdown(fd[0], SHUT_RDWR);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(sigaction(SIGUSR1, &previous, NULL) == 0);
  printf("interrupted write accepted=%d signals=%d filled=%u received=%u\n",
      accepted != 0, caught, sink.filled, sink.received);
  CHECK(caught > 0 && accepted && sink.valid && sink.received == sink.filled + 2);
  rfbClientCleanup(client); close(fd[1]);
}
int main(int argc, char** argv) {
  int fd[2]; char byte;
  uint64_t started;
  rfbClient* client;
  signal(SIGPIPE, SIG_IGN);
  if (argc == 2) {
    if (!strcmp(argv[1], "signal-short")) interrupted(2, TRUE, FALSE);
    else if (!strcmp(argv[1], "signal-large")) interrupted(307200, TRUE, FALSE);
    else if (!strcmp(argv[1], "signal-timeout")) interrupted(2, FALSE, FALSE);
    else if (!strcmp(argv[1], "signal-message")) interrupted(2, FALSE, TRUE);
    else if (!strcmp(argv[1], "signal-write")) interrupted_write();
    else CHECK(!"unknown case");
    return 0;
  }
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
  started = millis();
  CHECK(!ReadFromRFBServer(client, &byte, 1));
  CHECK(millis() - started < 250);
  rfbClientCleanup(client); close(fd[1]);

  client = rfbGetClient(8, 3, 4);
  client->sock = RFB_INVALID_SOCKET;
  CHECK(WaitForMessage(client, 1000) == -1);
  client->serverPort = -1;
  CHECK(WaitForMessage(client, 1000000) == 1);
  client->serverPort = 5900;
  rfbClientCleanup(client);
  interrupted(2, TRUE, FALSE);
  interrupted(307200, TRUE, FALSE);
  interrupted(2, FALSE, FALSE);
  interrupted(2, FALSE, TRUE);
  interrupted_write();
  puts("PASS buffered, fragmented, cumulative idle, unlimited, EOF, invalid fd and replay reads");
  return 0;
}
