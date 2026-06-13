// gcc-notch.c — PhobGCC-style software notch remapper for a Mayflash GameCube
// adapter (or any generic HID GC pad). Grabs the real evdev device, applies a
// piecewise-affine notch warp to the control stick and C-stick, and re-emits a
// virtual gamepad via uinput.
//
// Build:
//   gcc -O2 -o gcc-notch gcc-notch.c $(pkg-config --cflags --libs libevdev) -lm
//
// Calibrate (writes ~/.config/gcc-notch/calib.conf):
//   ./gcc-notch --calibrate [/dev/input/eventN]
// Run:
//   ./gcc-notch [/dev/input/eventN]

#include <libevdev-1.0/libevdev/libevdev-uinput.h>
#include <linux/input-event-codes.h>
#include <stddef.h>
#include <sys/poll.h>
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <libevdev-1.0/libevdev/libevdev.h>

#define NUM_NOTCH 8
#define OUT_CENTER 128.0
#define OUT_R 127.0
#define DIAG_FRAC 0.70
#define DEG (M_PI / 180.0)

static const char *DEV_NAME = "mayflash limited GameCube Controller Adapter";
static const char *CFG_REL = ".config/gcc-notch/calib.conf";

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) {
  (void)s;
  g_stop = 1;
}

static int cur[ABS_CNT]; /* latest raw value per ABS code */

// Calibration data
struct stick_cal {
  int ax, ay;
  double cx, cy;
  double nx[NUM_NOTCH], ny[NUM_NOTCH];
};

// Runtime map
struct stick_map {
  int ax, ay;
  double cx, cy;
  int n;
  double ang[NUM_NOTCH];               /* sorted ascending */
  double vx[NUM_NOTCH], vy[NUM_NOTCH]; /* measured vectors, sorted */
  double wx[NUM_NOTCH], wy[NUM_NOTCH]; /* ideal vectors, paired */
  double M[NUM_NOTCH][2][2];           /* sector i: notch i..(i+1) */
  int amin, amax;
};

// Helpers
static void cfg_path(char *buf, size_t n) {
  const char *home = getenv("HOME");
  snprintf(buf, n, "%s/%s", home ? home : ".", CFG_REL);
}

static void ensure_parent_dir(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
}

static int open_device(const char *arg, struct libevdev **out) {
  char path[256];
  int fd;
  if (arg) {
    fd = open(arg, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      perror("open");
      return -1;
    }
    if (libevdev_new_from_fd(fd, out) < 0) {
      fprintf(stderr, "libevdev init failed\n");
      close(fd);
      return -1;
    }
    return fd;
  }

  DIR *d = opendir("/dev/input");
  if (!d) {
    perror("opendir");
    return -1;
  }

  struct dirent *e;

  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "event", 5))
      continue;
    snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) == 0) {
      const char *nm = libevdev_get_name(dev);
      if (nm && strcmp(nm, DEV_NAME) == 0 &&
          libevdev_has_event_code(dev, EV_ABS, ABS_X)) {
        fprintf(stderr, "using %s (%s)\n", path, nm);
        *out = dev;
        closedir(d);
        return fd;
      }
      libevdev_free(dev);
    }
    close(fd);
  }
  closedir(d);
  fprintf(stderr, "no matching device; pass /dev/input/eventN explicitely\n");
  return -1;
}

static void drain(struct libevdev *dev) {
  struct input_event ev;
  int rc;
  while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0) {
    if (rc == LIBEVDEV_READ_STATUS_SYNC)
      continue;
    if (ev.type == EV_ABS && ev.code < ABS_CNT)
      cur[ev.code] = ev.value;
  }
}

/* Block until the user presses Enter, draining the device meanwhile so cur[]
 * stays fresh. */
static void wait_enter(struct libevdev *dev) {
  struct pollfd pfd[2] = {{libevdev_get_fd(dev), POLLIN, 0},
                          {STDIN_FILENO, POLLIN, 0}};
  for (;;) {
    if (poll(pfd, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      exit(1);
    }
    if (pfd[0].revents & POLLIN)
      drain(dev);
    if (pfd[1].revents & POLLIN) {
      int c;
      while ((c = getchar()) != '\n' && c != EOF) {
      }
      drain(dev);
      return;
    }
  }
}

// Calibration
static void calibrate_stick(struct libevdev *dev, const char *label,
                            struct stick_cal *sc) {
  printf("\n########## Calibrating %s stick ##########\n", label);

  // 1. Auto-detect this stick's two ABS codes via travel during a spin
  printf("Spin the %s stick slowly around its full gate a couple of "
         "times,\nthen press Enter...\n",
         label);
  int lo[6], hi[6];
  for (int i = 0; i < 6; i++) {
    lo[i] = INT_MAX;
    hi[i] = INT_MIN;
  }
  {
    struct pollfd pfd[2] = {{libevdev_get_fd(dev), POLLIN, 0},
                            {STDIN_FILENO, POLLIN, 0}};
    bool done = false;
    while (!done) {
      if (poll(pfd, 2, -1) < 0) {
        if (errno == EINTR)
          continue;
        perror("poll");
        exit(1);
      }
      if (pfd[0].revents & POLLIN) {
        struct input_event ev;
        int rc;
        while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL,
                                         &ev)) >= 0) {
          if (rc == LIBEVDEV_READ_STATUS_SYNC)
            continue;
          if (ev.type == EV_ABS && ev.code < ABS_CNT)
            cur[ev.code] = ev.value;
          if (ev.type == EV_ABS && ev.code < 6) {
            if (ev.value < lo[ev.code])
              lo[ev.code] = ev.value;
            if (ev.value > hi[ev.code])
              hi[ev.code] = ev.value;
          }
        }
      }
      if (pfd[1].revents & POLLIN) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {
        }
        done = true;
      }
    }
    int b1 = -1, b2 = -1, r1 = -1, r2 = -1;
    for (int i = 0; i < 6; i++) {
      int r = (hi[i] >= lo[i]) ? hi[i] - lo[i] : 0;
      if (r > r1) {
        r2 = r1;
        b2 = b1;
        r1 = r;
        b1 = i;
      } else if (r > r2) {
        r2 = r;
        b2 = i;
      }
    }
    if (b1 < b2) {
      sc->ax = b1;
      sc->ay = b2;
    } else {
      sc->ax = b2;
      sc->ay = b1;
    }
    printf("Detected %s axes: ABS %d & %d (travel %d, %d)\n", label, sc->ax,
           sc->ay, r1, r2);
  }

  // 2. center
  printf("Let the %s stick rest at center, then press Enter...\n", label);
  wait_enter(dev);
  sc->cx = cur[sc->ax];
  sc->cy = cur[sc->ay];
  printf("Center = (%.0f, %.0f)\n", sc->cx, sc->cy);

  // 3. eight notches, any order (classified later by angle)
  for (int i = 0; i < NUM_NOTCH; i++) {
    printf("Push the %s stick HARD into a notch and hold (%d/%d), Enter...\n",
           label, i + 1, NUM_NOTCH);
    wait_enter(dev);
    sc->nx[i] = cur[sc->ax];
    sc->ny[i] = cur[sc->ay];
    printf("  notch %d = (%.0f, %.0f)\n", i + 1, sc->nx[i], sc->ny[i]);
  }
}

// config I/O
static int save_cfg(const struct stick_cal *ctrl, const struct stick_cal *cst) {
  char path[512];
  cfg_path(path, sizeof path);
  ensure_parent_dir(path);
  FILE *f = fopen(path, "w");
  if (!f) {
    perror("fopen");
    return -1;
  }
  const struct stick_cal *s[2] = {ctrl, cst};
  const char *nm[2] = {"control", "cstick"};
  for (int k = 0; k < 2; k++) {
    fprintf(f, "%s axes %d %d\n", nm[k], s[k]->ax, s[k]->ay);
    fprintf(f, "%s center %.3f %.3f\n", nm[k], s[k]->cx, s[k]->cy);
    fprintf(f, "%s notch", nm[k]);
    for (int i = 0; i < NUM_NOTCH; i++)
      fprintf(f, " %.3f %.3f", s[k]->nx[i], s[k]->ny[i]);
    fprintf(f, "\n");
  }
  fclose(f);
  printf("Saved %s\n", path);
  return 0;
}

static struct stick_cal *sect(const char *s, struct stick_cal *c,
                              struct stick_cal *k) {
  if (!strcmp(s, "control"))
    return c;
  if (!strcmp(s, "cstick"))
    return k;
  return NULL;
}

static int load_cfg(struct stick_cal *ctrl, struct stick_cal *cst) {
  char path[512];
  cfg_path(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "no config at %s - run --calibrate first\n", path);
    return -1;
  }
  char a[64], b[64];
  int got_ctrl = 0, got_cst = 0;
  while (fscanf(f, "%63s %63s", a, b) == 2) {
    struct stick_cal *sc = sect(a, ctrl, cst);
    char line[512];
    if (!sc) {
      if (fgets(line, sizeof line, f)) {
      }
      continue;
    }
    if (!strcmp(b, "axes")) {
      if (fscanf(f, "%d %d", &sc->ax, &sc->ay) != 2)
        goto bad;
    } else if (!strcmp(b, "center")) {
      if (fscanf(f, "%lf %lf", &sc->cx, &sc->cy) != 2)
        goto bad;
    } else if (!strcmp(b, "notch")) {
      for (int i = 0; i < NUM_NOTCH; i++)
        if (fscanf(f, "%lf %lf", &sc->nx[i], &sc->ny[i]) != 2)
          goto bad;
      if (sc == ctrl)
        got_ctrl = 1;
      else
        got_cst = 1;
    } else {
      if (fgets(line, sizeof line, f)) {
      }
    }
  }
  fclose(f);
  if (!got_ctrl || !got_cst) {
    fprintf(stderr, "config missing a stick - re-run --calibrate\n");
    return -1;
  }
  return 0;
bad:
  fclose(f);
  fprintf(stderr, "config parse error\n");
  return -1;
}

// map build
static void canonical_target(int k, double *tx, double *ty) {
  double a = k * 45.0 * DEG, cx = cos(a), cy = sin(a);
  if (k % 2 == 0) { /* cardinal -> snap to an axis */
    *tx = OUT_R * round(cx);
    *ty = OUT_R * round(cy);
  } else { /* diagonal -> equal components */
    *tx = DIAG_FRAC * OUT_R * (cx > 0 ? 1.0 : -1.0);
    *ty = DIAG_FRAC * OUT_R * (cy > 0 ? 1.0 : -1.0);
  }
}

static void build_map(const struct stick_cal *sc, struct stick_map *mp,
                      int amin, int amax) {
  mp->ax = sc->ax;
  mp->ay = sc->ay;
  mp->cx = sc->cx;
  mp->cy = sc->cy;
  mp->n = NUM_NOTCH;
  mp->amin = amin;
  mp->amax = amax;

  double vx[NUM_NOTCH], vy[NUM_NOTCH], ang[NUM_NOTCH];
  int idx[NUM_NOTCH];
  for (int i = 0; i < NUM_NOTCH; i++) {
    vx[i] = sc->nx[i] - sc->cx;
    vy[i] = sc->ny[i] - sc->cy;
    ang[i] = atan2(vy[i], vx[i]);
    idx[i] = i;
  }
  for (int i = 1; i < NUM_NOTCH; i++) { /* insertion sort by angle */
    int t = idx[i];
    double key = ang[t];
    int j = i - 1;
    while (j >= 0 && ang[idx[j]] > key) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = t;
  }
  for (int i = 0; i < NUM_NOTCH; i++) {
    int s = idx[i];
    mp->vx[i] = vx[s];
    mp->vy[i] = vy[s];
    mp->ang[i] = ang[s];
  }
  /* align ideal octagon to the measured one in rotational order */
  int k0 = (int)llround(mp->ang[0] / (45.0 * DEG)) % 8;
  if (k0 < 0)
    k0 += 8;
  for (int i = 0; i < NUM_NOTCH; i++)
    canonical_target((k0 + i) % 8, &mp->wx[i], &mp->wy[i]);

  for (int i = 0; i < NUM_NOTCH; i++) {
    int j = (i + 1) % NUM_NOTCH;
    double a = mp->vx[i], b = mp->vx[j], c = mp->vy[i], d = mp->vy[j];
    double det = a * d - b * c;
    if (fabs(det) < 1e-6) {
      mp->M[i][0][0] = 1;
      mp->M[i][0][1] = 0;
      mp->M[i][1][0] = 0;
      mp->M[i][1][1] = 1;
      continue;
    }
    double i00 = d / det, i01 = -b / det, i10 = -c / det, i11 = a / det;
    double w00 = mp->wx[i], w01 = mp->wx[j], w10 = mp->wy[i], w11 = mp->wy[j];
    mp->M[i][0][0] = w00 * i00 + w01 * i10;
    mp->M[i][0][1] = w00 * i01 + w01 * i11;
    mp->M[i][1][0] = w10 * i00 + w11 * i10;
    mp->M[i][1][1] = w10 * i01 + w11 * i11;
  }
}

static void remap(const struct stick_map *mp, int rx, int ry, int *ox,
                  int *oy) {
  double sx = rx - mp->cx, sy = ry - mp->cy;
  double phi = atan2(sy, sx);
  int sec = mp->n - 1; /* wrap sector by default */
  for (int j = 0; j < mp->n - 1; j++)
    if (phi >= mp->ang[j] && phi < mp->ang[j + 1]) {
      sec = j;
      break;
    }
  double x = mp->M[sec][0][0] * sx + mp->M[sec][0][1] * sy + OUT_CENTER;
  double y = mp->M[sec][1][0] * sx + mp->M[sec][1][1] * sy + OUT_CENTER;
  int xo = (int)lround(x), yo = (int)lround(y);
  if (xo < mp->amin)
    xo = mp->amin;
  if (xo > mp->amax)
    xo = mp->amax;
  if (yo < mp->amin)
    yo = mp->amin;
  if (yo > mp->amax)
    yo = mp->amax;
  *ox = xo;
  *oy = yo;
}

// Main
int main(int argc, char *argv[]) {
  const char *devarg = NULL;
  bool docal = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--calibrate"))
      docal = true;
    else
      devarg = argv[i];
  }

  struct libevdev *dev = NULL;
  int fd = open_device(devarg, &dev);
  if (fd < 0)
    return 1;

  if (libevdev_grab(dev, LIBEVDEV_GRAB) < 0)
    fprintf(stderr, "warning: could not grab device (in use?)\n");

  for (int c = 0; c < ABS_CNT; c++) {
    if (libevdev_has_event_code(dev, EV_ABS, c))
      cur[c] = libevdev_get_event_value(dev, EV_ABS, c);
  }

  if (docal) {
    struct stick_cal ctrl, cst;
    memset(&ctrl, 0, sizeof(ctrl));
    memset(&cst, 0, sizeof(cst));
    calibrate_stick(dev, "control", &ctrl);
    calibrate_stick(dev, "cstick", &cst);
    save_cfg(&ctrl, &cst);
    libevdev_grab(dev, LIBEVDEV_UNGRAB);
    libevdev_free(dev);
    close(fd);
    return 0;
  }

  struct stick_cal ctrl, cst;
  memset(&ctrl, 0, sizeof ctrl);
  memset(&cst, 0, sizeof cst);
  if (load_cfg(&ctrl, &cst) < 0) {
    libevdev_free(dev);
    close(fd);
    return 1;
  }

  int amin = 0, amax = 255;
  if (libevdev_has_event_code(dev, EV_ABS, ABS_X)) {
    amin = libevdev_get_abs_minimum(dev, ABS_X);
    amax = libevdev_get_abs_maximum(dev, ABS_X);
  }
  struct stick_map mctrl, mcst;
  build_map(&ctrl, &mctrl, amin, amax);
  build_map(&cst, &mcst, amin, amax);

  bool is_stick[ABS_CNT];
  memset(is_stick, 0, sizeof is_stick);
  is_stick[ctrl.ax] = is_stick[ctrl.ay] = true;
  is_stick[cst.ax] = is_stick[cst.ay] = true;

  libevdev_set_name(dev, "GCC Notch Remap");
  /* Don't advertise force feedback on the virtual device: we don't service
   * uinput FF upload requests, and apps (SDL2) crash/hang trying to use it. */
  libevdev_disable_event_type(dev, EV_FF);
  libevdev_disable_event_type(dev, EV_FF_STATUS);
  struct libevdev_uinput *ui = NULL;
  if (libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED,
                                         &ui) < 0) {
    perror("uinput create");
    libevdev_free(dev);
    close(fd);
    return 1;
  }

  fprintf(stderr, "remapping -> %s (Ctrl-C to stop)\n",
          libevdev_uinput_get_devnode(ui));

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  struct pollfd pfd = {libevdev_get_fd(dev), POLLIN, 0};
  while (!g_stop) {
    if (poll(&pfd, 1, -1) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    struct input_event ev;
    int rc;
    while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >=
           0) {
      if (rc == LIBEVDEV_READ_STATUS_SYNC)
        continue;
      if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        int ox, oy;
        remap(&mctrl, cur[ctrl.ax], cur[ctrl.ay], &ox, &oy);
        libevdev_uinput_write_event(ui, EV_ABS, ctrl.ax, ox);
        libevdev_uinput_write_event(ui, EV_ABS, ctrl.ay, oy);
        remap(&mcst, cur[cst.ax], cur[cst.ay], &ox, &oy);
        libevdev_uinput_write_event(ui, EV_ABS, cst.ax, ox);
        libevdev_uinput_write_event(ui, EV_ABS, cst.ay, oy);
        libevdev_uinput_write_event(ui, EV_SYN, SYN_REPORT, 0);
      } else if (ev.type == EV_ABS) {
        if (ev.code < ABS_CNT)
          cur[ev.code] = ev.value;
        if (!(ev.code < ABS_CNT && is_stick[ev.code]))
          libevdev_uinput_write_event(ui, ev.type, ev.code, ev.value);
      } else if (ev.type != EV_SYN) {
        libevdev_uinput_write_event(ui, ev.type, ev.code, ev.value);
      }
    }
    if (rc == -ENODEV) {
      fprintf(stderr, "device disconnected\n");
      break;
    }
  }

  libevdev_uinput_destroy(ui);
  libevdev_grab(dev, LIBEVDEV_UNGRAB);
  libevdev_free(dev);
  close(fd);
  return 0;
}
