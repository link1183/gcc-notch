#define _GNU_SOURCE
#include "livesplit.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LS_PORT 16834
#define LOG_CAP 64
#define LINE_MAX 160

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t th;
static volatile bool running;
static int listen_fd = -1;
static bool listening_ok;
static bool client_on;
static long total_lines;

static char logbuf[LOG_CAP][LINE_MAX];
static int log_head, log_count;

/* parsed run state */
static bool run_active;
static long start_seq, end_seq;
static long game_ms, final_ms;
static double last_update; /* monotonic time of last setgametime */

#define IDLE_END_SEC 2.0 /* setgametime gap that counts as run end */

static double mono(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void parse_line(const char *s) {
  /* the setgametime stream is high-rate: track the time, but keep it out of the
     command log so it doesn't drown everything else */
  if (!strncmp(s, "setgametime", 11)) {
    unsigned h, m, se, ms;
    if (sscanf(s, "setgametime %u:%u:%u.%u", &h, &m, &se, &ms) == 4) {
      pthread_mutex_lock(&mtx);
      game_ms = ((long)h * 3600 + (long)m * 60 + se) * 1000L + ms;
      last_update = mono();
      pthread_mutex_unlock(&mtx);
    }
    return;
  }
  pthread_mutex_lock(&mtx);
  snprintf(logbuf[log_head], LINE_MAX, "%s", s);
  log_head = (log_head + 1) % LOG_CAP;
  if (log_count < LOG_CAP)
    log_count++;
  total_lines++;
  if (!strcmp(s, "starttimer")) {
    if (run_active) { /* no stop command exists; close the previous run */
      end_seq++;
      final_ms = game_ms;
    }
    run_active = true;
    start_seq++;
    game_ms = 0;
    last_update = mono(); /* grace period before the first setgametime */
  }
  pthread_mutex_unlock(&mtx);
  fprintf(stderr, "[livesplit] %s\n", s); /* mirror to the terminal */
}

/* a run ends silently (the timer just stops updating), so treat a gap in the
   setgametime stream as the run finishing */
static void check_idle(void) {
  pthread_mutex_lock(&mtx);
  if (run_active && mono() - last_update > IDLE_END_SEC) {
    run_active = false;
    end_seq++;
    final_ms = game_ms;
  }
  pthread_mutex_unlock(&mtx);
}

/* read a connected client until it closes or we're asked to stop, splitting the
   byte stream into newline-terminated commands */
static void handle_client(int fd) {
  pthread_mutex_lock(&mtx);
  client_on = true;
  pthread_mutex_unlock(&mtx);
  fprintf(stderr, "[livesplit] client connected\n");

  char acc[LINE_MAX];
  size_t acclen = 0;
  char buf[512];
  while (running) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int r = poll(&p, 1, 200);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r > 0) {
      ssize_t n = recv(fd, buf, sizeof buf, 0);
      if (n <= 0)
        break; /* peer closed or error */
      for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
          if (acclen > 0) {
            acc[acclen] = 0;
            parse_line(acc);
            acclen = 0;
          }
        } else if (acclen < LINE_MAX - 1) {
          acc[acclen++] = c;
        }
      }
    }
    check_idle(); /* also fires on poll timeouts, when the stream is quiet */
  }
  close(fd);
  pthread_mutex_lock(&mtx);
  if (run_active) { /* client vanished mid-run: close it out */
    run_active = false;
    end_seq++;
    final_ms = game_ms;
  }
  client_on = false;
  pthread_mutex_unlock(&mtx);
  fprintf(stderr, "[livesplit] client disconnected\n");
}

static void *server_loop(void *arg) {
  (void)arg;
  while (running) {
    struct pollfd p = {.fd = listen_fd, .events = POLLIN};
    int r = poll(&p, 1, 200);
    if (r <= 0)
      continue;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
      continue;
    handle_client(fd); /* one client at a time is plenty for a timer link */
  }
  return NULL;
}

void ls_start(void) {
  if (running)
    return;
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
    return;
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(LS_PORT);
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    fprintf(stderr, "[livesplit] bind 127.0.0.1:%d failed: %s\n", LS_PORT,
            strerror(errno));
    close(listen_fd);
    listen_fd = -1;
    return;
  }
  if (listen(listen_fd, 1) < 0) {
    close(listen_fd);
    listen_fd = -1;
    return;
  }
  listening_ok = true;
  running = true;
  if (pthread_create(&th, NULL, server_loop, NULL) != 0) {
    running = false;
    listening_ok = false;
    close(listen_fd);
    listen_fd = -1;
    return;
  }
  fprintf(stderr, "[livesplit] listening on 127.0.0.1:%d\n", LS_PORT);
}

void ls_stop(void) {
  if (!running)
    return;
  running = false;
  if (listen_fd >= 0)
    shutdown(listen_fd, SHUT_RDWR); /* nudge accept()/poll() awake */
  pthread_join(th, NULL);
  if (listen_fd >= 0) {
    close(listen_fd);
    listen_fd = -1;
  }
  listening_ok = false;
}

bool ls_listening(void) { return listening_ok; }

#define LS_GETTER(fn, type, field)                                             \
  type fn(void) {                                                              \
    pthread_mutex_lock(&mtx);                                                  \
    type v = (field);                                                          \
    pthread_mutex_unlock(&mtx);                                                \
    return v;                                                                  \
  }

LS_GETTER(ls_connected, bool, client_on)
LS_GETTER(ls_total_lines, long, total_lines)
LS_GETTER(ls_log_count, int, log_count)

void ls_log_copy(int i, char *out, size_t n) {
  if (!out || n == 0)
    return;
  out[0] = 0;
  pthread_mutex_lock(&mtx);
  if (i >= 0 && i < log_count) {
    int oldest = (log_head - log_count + LOG_CAP) % LOG_CAP;
    snprintf(out, n, "%s", logbuf[(oldest + i) % LOG_CAP]);
  }
  pthread_mutex_unlock(&mtx);
}

LS_GETTER(ls_run_active, bool, run_active)
LS_GETTER(ls_start_seq, long, start_seq)
LS_GETTER(ls_end_seq, long, end_seq)
LS_GETTER(ls_game_ms, long, game_ms)
LS_GETTER(ls_final_ms, long, final_ms)
