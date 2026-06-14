#include <asm-generic/errno-base.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <sys/stat.h>
#define _GNU_SOURCE
#include "engine.h"
#include <dirent.h>
#include <fcntl.h>
#include <libevdev-1.0/libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUT_CENTER 128.0
#define OUT_R 127.0
#define DEG (M_PI / 180.0)

static const char *DEV_NAME = "mayflash limited GameCube Controller Adapter";
static const char *CFG_REL = ".config/gcc-notch/calib.conf";

typedef struct {
  int ax, ay;
  double cx, cy;
  double ang[ENG_NOTCH], vx[ENG_NOTCH], vy[ENG_NOTCH];
  double wx[ENG_NOTCH], wy[ENG_NOTCH];
  double M[ENG_NOTCH][2][2];
  int amin, amax;
} smap;

static struct libevdev *dev;
static int devfd = -1;
static struct libevdev_uinput *ui;
static bool remapping, connected;
static bool want_remap; /* user intent: keep remapping across reconnects */
static long ev_count;   /* cumulative EV_ABS/EV_KEY events, for rate readout */

static int cur[ABS_CNT];
static int keyst[KEY_CNT];

static eng_stick_cal cal[2];
static smap map[2];
static bool have_cal;
static bool is_stick[ABS_CNT];
static double diag_frac[2] = {0.70, 0.70};
static double deadzone = 0.0; /* center deadzone, fraction of stick radius */
static double trig_dz = 0.0;  /* trigger bottom deadzone, fraction of range */
static int amin = 0, amax = 255;

/* analog trigger rescaling: map measured [lo,hi] onto the axis full range */
static int trig_lo[ABS_CNT], trig_hi[ABS_CNT];
static bool trig_on[ABS_CNT];
static bool have_trig;
static bool trig_active;
static int trig_phase; /* 0 = capture rest, 1 = capture full press */
static int t_lo[ABS_CNT], t_hi[ABS_CNT];

/* GameCube button map for the stream viewer */
#define GC_N 8
static const char *GC_NAMES[GC_N] = {"A", "B", "X", "Y",
                                     "Z", "L", "R", "Start"};
static int gc_btn[GC_N]; /* evdev BTN code per GC button, -1 = none */
static bool have_btnmap; /* a mapping has been captured/loaded       */
static int dpad_x = -1, dpad_y = -1; /* ABS hat axes for the D-pad */
static bool map_active; /* mapping wizard running                  */
static int map_idx;     /* which GC button we're capturing         */

static bool cal_active;
static int cal_stick, cal_phase, cal_notch;
static int cal_cand[2] = {-1, -1};
static eng_stick_cal tmp[2];
static int lo6[6], hi6[6];

static char devpaths[16][64];
static int devcount, devidx;

static void remap_teardown(void); /* defined with the remap controls below */
static void btnmap_advance(void); /* defined with the mapping wizard below */

/* ---------- math ---------- */
static void canonical_target(int k, double diag, double *tx, double *ty) {
  double a = k * 45.0 * DEG, c = cos(a), s = sin(a);
  if (k % 2 == 0) {
    *tx = OUT_R * round(c);
    *ty = OUT_R * round(s);
  } else {
    *tx = diag * OUT_R * (c > 0 ? 1 : -1);
    *ty = diag * OUT_R * (s > 0 ? 1 : -1);
  }
}

static void build_map(const eng_stick_cal *sc, smap *mp, double diag) {
  mp->ax = sc->ax;
  mp->ay = sc->ay;
  mp->cx = sc->cx;
  mp->cy = sc->cy;
  mp->amin = amin;
  mp->amax = amax;
  double vx[ENG_NOTCH], vy[ENG_NOTCH], ang[ENG_NOTCH];
  int idx[ENG_NOTCH];
  for (int i = 0; i < ENG_NOTCH; i++) {
    vx[i] = sc->nx[i] - sc->cx;
    vy[i] = sc->ny[i] - sc->cy;
    ang[i] = atan2(vy[i], vx[i]);
    idx[i] = i;
  }
  for (int i = 1; i < ENG_NOTCH; i++) {
    int t = idx[i];
    double k = ang[t];
    int j = i - 1;
    while (j >= 0 && ang[idx[j]] > k) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = t;
  }
  for (int i = 0; i < ENG_NOTCH; i++) {
    int s = idx[i];
    mp->vx[i] = vx[s];
    mp->vy[i] = vy[s];
    mp->ang[i] = ang[s];
  }
  int k0 = (int)llround(mp->ang[0] / (45.0 * DEG)) % 8;
  if (k0 < 0)
    k0 += 8;
  for (int i = 0; i < ENG_NOTCH; i++)
    canonical_target((k0 + i) % 8, diag, &mp->wx[i], &mp->wy[i]);
  for (int i = 0; i < ENG_NOTCH; i++) {
    int j = (i + 1) % ENG_NOTCH;
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

static void remap(const smap *mp, int rx, int ry, int *ox, int *oy) {
  double sx = rx - mp->cx, sy = ry - mp->cy;
  /* radial center deadzone: snap to center inside, ramp magnitude outside */
  double dz = deadzone * (mp->amax - mp->amin) / 2.0;
  double r = hypot(sx, sy);
  if (r <= dz) {
    *ox = *oy = (int)lround(OUT_CENTER);
    return;
  }
  if (dz > 0) {
    double k = (r - dz) / r;
    sx *= k;
    sy *= k;
  }
  double phi = atan2(sy, sx);
  int sec = ENG_NOTCH - 1;
  for (int j = 0; j < ENG_NOTCH - 1; j++)
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

static void recompute_is_stick(void) {
  memset(is_stick, 0, sizeof is_stick);
  for (int s = 0; s < 2; s++) {
    if (cal[s].ax >= 0 && cal[s].ax < ABS_CNT)
      is_stick[cal[s].ax] = true;
    if (cal[s].ay >= 0 && cal[s].ay < ABS_CNT)
      is_stick[cal[s].ay] = true;
  }
}

static void rebuild_maps(void) {
  build_map(&cal[0], &map[0], diag_frac[0]);
  build_map(&cal[1], &map[1], diag_frac[1]);
  recompute_is_stick();
}

/* an analog (non-stick) axis worth treating as a trigger: real range, not a
 * digital hat */
static bool is_analog_abs(int c) {
  if (c < 0 || c >= ABS_CNT || !dev || is_stick[c])
    return false;
  if (!libevdev_has_event_code(dev, EV_ABS, c))
    return false;
  return libevdev_get_abs_maximum(dev, c) - libevdev_get_abs_minimum(dev, c) >=
         16;
}

/* rescale a raw trigger reading so calibrated rest..full spans the axis range
 */
static int trig_apply(int code, int v) {
  if (code < 0 || code >= ABS_CNT || !trig_on[code] || !dev)
    return v;
  int lo = trig_lo[code], hi = trig_hi[code];
  if (hi <= lo)
    return v;
  int dmin = libevdev_get_abs_minimum(dev, code);
  int dmax = libevdev_get_abs_maximum(dev, code);
  double f = (double)(v - lo) / (hi - lo);
  if (f < 0)
    f = 0;
  if (f > 1)
    f = 1;
  if (trig_dz > 0)
    f = (f <= trig_dz) ? 0.0 : (f - trig_dz) / (1.0 - trig_dz);
  return (int)lround(dmin + f * (dmax - dmin));
}

static void emit_sticks(void) {
  int ox, oy;
  remap(&map[0], cur[cal[0].ax], cur[cal[0].ay], &ox, &oy);
  libevdev_uinput_write_event(ui, EV_ABS, cal[0].ax, ox);
  libevdev_uinput_write_event(ui, EV_ABS, cal[0].ay, oy);
  remap(&map[1], cur[cal[1].ax], cur[cal[1].ay], &ox, &oy);
  libevdev_uinput_write_event(ui, EV_ABS, cal[1].ax, ox);
  libevdev_uinput_write_event(ui, EV_ABS, cal[1].ay, oy);
  libevdev_uinput_write_event(ui, EV_SYN, SYN_REPORT, 0);
}

/* ---------- config ---------- */
static void cfg_path(char *b, size_t n) {
  const char *h = getenv("HOME");
  snprintf(b, n, "%s/%s", h ? h : ".", CFG_REL);
}

static void ensure_parent(const char *p) {
  char t[256];
  snprintf(t, sizeof t, "%s", p);
  for (char *q = t + 1; *q; q++)
    if (*q == '/') {
      *q = 0;
      mkdir(t, 0755);
      *q = '/';
    }
}

static bool save_to(const char *p) {
  ensure_parent(p);
  FILE *f = fopen(p, "w");
  if (!f)
    return false;
  const char *nm[2] = {"control", "cstick"};
  for (int k = 0; k < 2; k++) {
    fprintf(f, "%s axes %d %d\n", nm[k], cal[k].ax, cal[k].ay);
    fprintf(f, "%s center %.3f %.3f\n", nm[k], cal[k].cx, cal[k].cy);
    fprintf(f, "%s notch", nm[k]);
    for (int i = 0; i < ENG_NOTCH; i++)
      fprintf(f, " %.3f %.3f", cal[k].nx[i], cal[k].ny[i]);
    fprintf(f, "\n");
    fprintf(f, "%s diag %.4f\n", nm[k], diag_frac[k]);
  }
  fprintf(f, "global deadzone %.4f\n", deadzone);
  fprintf(f, "global trigdz %.4f\n", trig_dz);
  for (int c = 0; c < ABS_CNT; c++)
    if (trig_on[c])
      fprintf(f, "trigger %d %d %d\n", c, trig_lo[c], trig_hi[c]);
  for (int i = 0; i < GC_N; i++)
    if (gc_btn[i] >= 0)
      fprintf(f, "gcbtn %d %d\n", i, gc_btn[i]);
  if (dpad_x >= 0 || dpad_y >= 0)
    fprintf(f, "dpad %d %d\n", dpad_x, dpad_y);
  fclose(f);
  return true;
}

static bool load_from(const char *p) {
  FILE *f = fopen(p, "r");
  if (!f)
    return false;
  /* reset to defaults so a partial profile doesn't inherit prior values */
  for (int c = 0; c < ABS_CNT; c++)
    trig_on[c] = false;
  have_trig = false;
  have_cal = false;
  diag_frac[0] = diag_frac[1] = 0.70;
  deadzone = 0.0;
  trig_dz = 0.0;
  cal[0].ax = 0, cal[0].ay = 1, cal[1].ax = 5, cal[1].ay = 2;
  for (int i = 0; i < GC_N; i++)
    gc_btn[i] = -1;
  have_btnmap = false;
  dpad_x = dpad_y = -1;
  char a[64], b[64];
  int gc = 0, gk = 0;
  while (fscanf(f, "%63s %63s", a, b) == 2) {
    if (!strcmp(a, "trigger")) {
      int code = atoi(b), lo, hi;
      if (fscanf(f, "%d %d", &lo, &hi) == 2 && code >= 0 && code < ABS_CNT) {
        trig_lo[code] = lo;
        trig_hi[code] = hi;
        trig_on[code] = true;
        have_trig = true;
      }
      continue;
    }
    if (!strcmp(a, "global")) {
      double val;
      if (fscanf(f, "%lf", &val) == 1) {
        if (!strcmp(b, "deadzone"))
          deadzone = val;
        else if (!strcmp(b, "trigdz"))
          trig_dz = val;
      }
      continue;
    }
    if (!strcmp(a, "gcbtn")) {
      int idx = atoi(b), code;
      if (fscanf(f, "%d", &code) == 1 && idx >= 0 && idx < GC_N) {
        gc_btn[idx] = code;
        have_btnmap = true;
      }
      continue;
    }
    if (!strcmp(a, "dpad")) {
      int yy;
      if (fscanf(f, "%d", &yy) == 1) {
        dpad_x = atoi(b);
        dpad_y = yy;
      }
      continue;
    }
    int s = !strcmp(a, "control") ? 0 : !strcmp(a, "cstick") ? 1 : -1;
    char ln[256];
    if (s < 0) {
      if (fgets(ln, sizeof ln, f)) {
      }
      continue;
    }
    if (!strcmp(b, "axes")) {
      if (fscanf(f, "%d %d", &cal[s].ax, &cal[s].ay) != 2) {
        fclose(f);
        return false;
      }
    } else if (!strcmp(b, "center")) {
      if (fscanf(f, "%lf %lf", &cal[s].cx, &cal[s].cy) != 2) {
        fclose(f);
        return false;
      }
    } else if (!strcmp(b, "notch")) {
      for (int i = 0; i < ENG_NOTCH; i++)
        if (fscanf(f, "%lf %lf", &cal[s].nx[i], &cal[s].ny[i]) != 2) {
          fclose(f);
          return false;
        }
      if (s == 0)
        gc = 1;
      else
        gk = 1;
    } else if (!strcmp(b, "diag")) {
      if (fscanf(f, "%lf", &diag_frac[s]) != 1) {
        fclose(f);
        return false;
      }
    } else {
      if (fgets(ln, sizeof ln, f)) {
      }
    }
  }
  fclose(f);
  if (gc && gk) {
    have_cal = true;
    rebuild_maps();
    return true;
  }
  return false;
}

/* ---------- profiles ---------- */
static char cur_profile[64] = "default";

static char prof_names[32][64];

static int prof_count;

static void profiles_dir(char *b, size_t n) {
  const char *h = getenv("HOME");
  snprintf(b, n, "%s/.config/gcc-notch/profiles", h ? h : ".");
}

static void profile_path(const char *name, char *b, size_t n) {
  const char *h = getenv("HOME");
  snprintf(b, n, "%s/.config/gcc-notch/profiles/%s.conf", h ? h : ".", name);
}

static void active_path(char *b, size_t n) {
  const char *h = getenv("HOME");
  snprintf(b, n, "%s/.config/gcc-notch/active", h ? h : ".");
}

static bool is_conf(const char *nm) {
  size_t L = strlen(nm);
  return L > 5 && !strcmp(nm + L - 5, ".conf");
}

static void scan_profiles(void) {
  prof_count = 0;
  char dir[256];
  profiles_dir(dir, sizeof dir);
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && prof_count < 32)
    if (is_conf(e->d_name))
      snprintf(prof_names[prof_count++], 64, "%.*s",
               (int)(strlen(e->d_name) - 5), e->d_name);
  closedir(d);
}

/* one-time import of the legacy single-file config into profiles/default */
static void migrate_legacy(void) {
  char dir[256];
  profiles_dir(dir, sizeof dir);
  scan_profiles();
  if (prof_count > 0)
    return;
  char legacy[256];
  cfg_path(legacy, sizeof legacy);
  FILE *in = fopen(legacy, "r");
  if (!in)
    return;
  char dst[256];
  profile_path("default", dst, sizeof dst);
  ensure_parent(dst);
  FILE *out = fopen(dst, "w");
  if (out) {
    int ch;
    while ((ch = fgetc(in)) != EOF)
      fputc(ch, out);
    fclose(out);
  }
  fclose(in);
}

static void read_active(void) {
  snprintf(cur_profile, sizeof cur_profile, "default");
  char ap[256];
  active_path(ap, sizeof ap);
  FILE *f = fopen(ap, "r");
  if (f) {
    if (fscanf(f, "%63s", cur_profile) != 1)
      snprintf(cur_profile, sizeof cur_profile, "default");
    fclose(f);
  }
}

static void write_active(void) {
  char ap[256];
  active_path(ap, sizeof ap);
  ensure_parent(ap);
  FILE *f = fopen(ap, "w");
  if (f) {
    fprintf(f, "%s\n", cur_profile);
    fclose(f);
  }
}

bool eng_save_cfg(void) {
  char p[256];
  profile_path(cur_profile, p, sizeof p);
  return save_to(p);
}

bool eng_load_cfg(void) {
  migrate_legacy();
  read_active();
  char p[256];
  profile_path(cur_profile, p, sizeof p);
  return load_from(p);
}

int eng_profile_count(void) {
  scan_profiles();
  return prof_count;
}

const char *eng_profile_name(int i) {
  return (i >= 0 && i < prof_count) ? prof_names[i] : "";
}

const char *eng_profile_current(void) { return cur_profile; }

bool eng_profile_select(const char *name) {
  char p[256];
  profile_path(name, p, sizeof p);
  FILE *t = fopen(p, "r");
  if (!t)
    return false;
  fclose(t);
  load_from(p);
  snprintf(cur_profile, sizeof cur_profile, "%s", name);
  write_active();
  return true;
}

bool eng_profile_save_as(const char *name) {
  char p[256];
  profile_path(name, p, sizeof p);
  if (!save_to(p))
    return false;
  snprintf(cur_profile, sizeof cur_profile, "%s", name);
  write_active();
  return true;
}

void eng_profile_delete(const char *name) {
  char p[256];
  profile_path(name, p, sizeof p);
  remove(p);
  if (!strcmp(name, cur_profile)) {
    scan_profiles();
    if (prof_count > 0)
      eng_profile_select(prof_names[0]);
    else
      snprintf(cur_profile, sizeof cur_profile, "default");
  }
}

void eng_clear_cal(void) {
  have_cal = false;
  have_trig = false;
  for (int c = 0; c < ABS_CNT; c++)
    trig_on[c] = false;
  cal[0].ax = 0, cal[0].ay = 1, cal[1].ax = 5, cal[1].ay = 2;
  recompute_is_stick();
  eng_stop_remap();
  eng_save_cfg();
}

/* ---------- device ---------- */
static void seed_state(void) {
  for (int c = 0; c < ABS_CNT; c++)
    if (libevdev_has_event_code(dev, EV_ABS, c))
      cur[c] = libevdev_get_event_value(dev, EV_ABS, c);
  for (int k = 0; k < KEY_CNT; k++)
    if (libevdev_has_event_code(dev, EV_KEY, k))
      keyst[k] = libevdev_get_event_value(dev, EV_KEY, k);
  amin = libevdev_has_event_code(dev, EV_ABS, ABS_X)
             ? libevdev_get_abs_minimum(dev, ABS_X)
             : 0;
  amax = libevdev_has_event_code(dev, EV_ABS, ABS_X)
             ? libevdev_get_abs_maximum(dev, ABS_X)
             : 255;
}

static bool open_path(const char *path) {
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct libevdev *d = NULL;
  if (libevdev_new_from_fd(fd, &d) < 0) {
    close(fd);
    return false;
  }
  dev = d;
  devfd = fd;
  connected = true;
  seed_state();
  if (have_cal)
    rebuild_maps();
  return true;
}

static void scan_devices(void) {
  devcount = 0;
  DIR *dd = opendir("/dev/input");
  if (!dd)
    return;
  struct dirent *e;
  while ((e = readdir(dd)) && devcount < 16) {
    if (strncmp(e->d_name, "event", 5))
      continue;
    char path[286];
    snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    struct libevdev *d = NULL;
    if (libevdev_new_from_fd(fd, &d) == 0) {
      const char *nm = libevdev_get_name(d);
      if (nm && strcmp(nm, DEV_NAME) == 0 &&
          libevdev_has_event_code(d, EV_ABS, ABS_X))
        snprintf(devpaths[devcount++], 286, "%s", path);
      libevdev_free(d);
    }
    close(fd);
  }
  closedir(dd);
}
bool eng_open(const char *path) {
  /* sensible defaults so the viewer shows something before calibration */
  cal[0].ax = 0;
  cal[0].ay = 1;
  cal[1].ax = 5;
  cal[1].ay = 2;
  recompute_is_stick();
  scan_devices();
  if (path) {
    devidx = 0;
    return open_path(path);
  }
  if (devcount == 0)
    return false;
  devidx = 0;
  return open_path(devpaths[devidx]);
}
void eng_close(void) {
  eng_stop_remap();
  if (dev) {
    libevdev_free(dev);
    dev = NULL;
  }
  if (devfd >= 0) {
    close(devfd);
    devfd = -1;
  }
}

bool eng_connected(void) { return connected; }

long eng_event_count(void) { return ev_count; }

int eng_dev_count(void) { return devcount; }

const char *eng_dev_path(void) {
  return devcount ? devpaths[devidx] : "(none)";
}

void eng_dev_next(void) {
  if (devcount < 2)
    return;
  remap_teardown(); /* keep remap intent across the port switch */
  if (dev) {
    libevdev_free(dev);
    dev = NULL;
  }
  if (devfd >= 0) {
    close(devfd);
    devfd = -1;
  }
  devidx = (devidx + 1) % devcount;
  if (open_path(devpaths[devidx]) && want_remap && have_cal)
    eng_start_remap();
}

/* device vanished: drop it cleanly but remember whether we were remapping */
static void handle_disconnect(void) {
  remap_teardown();
  if (dev) {
    libevdev_free(dev);
    dev = NULL;
  }
  if (devfd >= 0) {
    close(devfd);
    devfd = -1;
  }
  connected = false;
}

/* throttled rescan; reopens the controller and resumes remap when it returns */
static void try_reconnect(void) {
  static double last;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double t = ts.tv_sec + ts.tv_nsec / 1e9;
  if (t - last < 0.5)
    return;
  last = t;
  scan_devices();
  if (devcount == 0)
    return;
  if (devidx >= devcount)
    devidx = 0;
  if (open_path(devpaths[devidx]) && want_remap && have_cal)
    eng_start_remap();
}

void eng_poll(void) {
  if (!dev) {
    try_reconnect();
    if (!dev)
      return;
  }
  struct input_event ev;
  int rc;
  bool emit = remapping && ui && !cal_active && !trig_active && !map_active;
  while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0) {
    if (rc == LIBEVDEV_READ_STATUS_SYNC)
      continue;
    if (ev.type == EV_ABS) {
      ev_count++;
      if (ev.code < ABS_CNT)
        cur[ev.code] = ev.value;
      if (emit && ev.code < ABS_CNT && !is_stick[ev.code])
        libevdev_uinput_write_event(ui, EV_ABS, ev.code,
                                    trig_apply(ev.code, ev.value));
    } else if (ev.type == EV_KEY) {
      ev_count++;
      if (ev.code < KEY_CNT)
        keyst[ev.code] = ev.value;
      /* mapping wizard: assign the first fresh button press to the cur step */
      if (map_active && ev.value == 1 && map_idx < GC_N) {
        bool used = false;
        for (int i = 0; i < map_idx; i++)
          if (gc_btn[i] == (int)ev.code)
            used = true;
        if (!used) {
          gc_btn[map_idx] = ev.code;
          btnmap_advance();
        }
      }
      if (emit)
        libevdev_uinput_write_event(ui, EV_KEY, ev.code, ev.value);
    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
      if (emit)
        emit_sticks();
    } else if (ev.type != EV_SYN) {
      if (emit)
        libevdev_uinput_write_event(ui, ev.type, ev.code, ev.value);
    }
  }
  if (rc == -ENODEV)
    handle_disconnect();
  if (cal_active && cal_phase == 0)
    for (int i = 0; i < 6; i++) {
      if (cur[i] < lo6[i])
        lo6[i] = cur[i];
      if (cur[i] > hi6[i])
        hi6[i] = cur[i];
    }
  if (trig_active)
    for (int c = 0; c < ABS_CNT; c++)
      if (is_analog_abs(c)) {
        if (cur[c] < t_lo[c])
          t_lo[c] = cur[c];
        if (cur[c] > t_hi[c])
          t_hi[c] = cur[c];
      }
}

/* ---------- accessors ---------- */
int eng_axis_min(void) { return amin; }

int eng_axis_max(void) { return amax; }

bool eng_has_cal(void) { return have_cal; }

const eng_stick_cal *eng_cal(int s) { return &cal[s]; }

void eng_measured_vec(int s, int i, double *vx, double *vy) {
  *vx = map[s].vx[i];
  *vy = map[s].vy[i];
}

void eng_ideal_vec(int s, int i, double *wx, double *wy) {
  *wx = map[s].wx[i];
  *wy = map[s].wy[i];
}

void eng_remap_point(int s, int rx, int ry, int *ox, int *oy) {
  remap(&map[s], rx, ry, ox, oy);
}

int eng_raw(int code) { return (code >= 0 && code < ABS_CNT) ? cur[code] : 0; }

bool eng_key(int code) {
  return (code >= 0 && code < KEY_CNT) ? keyst[code] != 0 : false;
}

int eng_list_keys(int *codes, int max) {
  if (!dev)
    return 0;
  int n = 0;
  for (int c = 0; c < KEY_CNT && n < max; c++)
    if (libevdev_has_event_code(dev, EV_KEY, c))
      codes[n++] = c;
  return n;
}

int eng_list_extra_abs(int *codes, int max) {
  if (!dev)
    return 0;
  int n = 0;
  for (int c = 0; c < ABS_CNT && n < max; c++)
    if (libevdev_has_event_code(dev, EV_ABS, c) && !is_stick[c])
      codes[n++] = c;
  return n;
}

const char *eng_code_name_key(int c) {
  const char *s = libevdev_event_code_get_name(EV_KEY, c);
  return s ? s : "?";
}

const char *eng_code_name_abs(int c) {
  const char *s = libevdev_event_code_get_name(EV_ABS, c);
  return s ? s : "?";
}

int eng_abs_min(int c) { return dev ? libevdev_get_abs_minimum(dev, c) : 0; }

int eng_abs_max(int c) { return dev ? libevdev_get_abs_maximum(dev, c) : 255; }

bool eng_remap_active(void) { return remapping; }

const char *eng_remap_devnode(void) {
  return (remapping && ui) ? libevdev_uinput_get_devnode(ui) : NULL;
}

/* tear down the uinput mirror without forgetting the user's intent */
static void remap_teardown(void) {
  if (ui) {
    libevdev_uinput_destroy(ui);
    ui = NULL;
  }
  if (dev)
    libevdev_grab(dev, LIBEVDEV_UNGRAB);
  remapping = false;
}
bool eng_start_remap(void) {
  if (!have_cal)
    return false;
  want_remap = true; /* intent persists even if the device isn't ready yet */
  if (!dev || remapping)
    return false;
  if (libevdev_grab(dev, LIBEVDEV_GRAB) < 0)
    return false;
  libevdev_set_name(dev, "GCC Notch Remap");
  libevdev_disable_event_type(dev, EV_FF);
  libevdev_disable_event_type(dev, EV_FF_STATUS);
  if (libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED,
                                         &ui) < 0) {
    libevdev_grab(dev, LIBEVDEV_UNGRAB);
    return false;
  }
  remapping = true;
  return true;
}

void eng_stop_remap(void) {
  want_remap = false; /* explicit user stop */
  remap_teardown();
}

double eng_get_diag(int s) { return diag_frac[s & 1]; }

void eng_set_diag(int s, double d) {
  diag_frac[s & 1] = d;
  if (have_cal)
    rebuild_maps();
}

double eng_get_deadzone(void) { return deadzone; }

void eng_set_deadzone(double d) { deadzone = d; }

double eng_get_trig_dz(void) { return trig_dz; }

void eng_set_trig_dz(double d) { trig_dz = d; }

/* ---------- calibration wizard ---------- */
void eng_cal_begin(void) {
  cal_active = true;
  cal_stick = 0;
  cal_phase = 0;
  cal_notch = 0;
  memset(tmp, 0, sizeof tmp);
  for (int i = 0; i < 6; i++) {
    lo6[i] = INT_MAX;
    hi6[i] = INT_MIN;
  }
}

bool eng_cal_active(void) { return cal_active; }

int eng_cal_stick(void) { return cal_stick; }

int eng_cal_phase(void) { return cal_phase; }

int eng_cal_notch(void) { return cal_notch; }

void eng_cal_cancel(void) { cal_active = false; }

void eng_cal_advance(void) {
  if (!cal_active)
    return;
  eng_stick_cal *t = &tmp[cal_stick];
  if (cal_phase == 0) {
    int b1 = -1, b2 = -1, r1 = -1, r2 = -1;
    for (int i = 0; i < 6; i++) {
      int r = (hi6[i] >= lo6[i]) ? hi6[i] - lo6[i] : 0;
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
    cal_cand[0] = b1;
    cal_cand[1] = b2;
    cal_phase = 1;
  } else if (cal_phase == 1) {
    int a = cal_cand[0], b = cal_cand[1];
    if (a >= 0 && b >= 0) {
      double ma = (lo6[a] + hi6[a]) / 2.0, mb = (lo6[b] + hi6[b]) / 2.0;
      if (fabs(cur[b] - mb) > fabs(cur[a] - ma)) {
        int s = a;
        a = b;
        b = s;
      }
      t->ax = a;
      t->ay = b;
    } else { /* detection incomplete: fall back to the old index heuristic */
      t->ax = (a < b) ? a : b;
      t->ay = (a < b) ? b : a;
    }
    cal_phase = 2;
  } else if (cal_phase == 2) {
    t->cx = cur[t->ax];
    t->cy = cur[t->ay];
    cal_phase = 3;
    cal_notch = 0;
  } else {
    t->nx[cal_notch] = cur[t->ax];
    t->ny[cal_notch] = cur[t->ay];
    cal_notch++;
    if (cal_notch >= ENG_NOTCH) {
      if (cal_stick == 0) {
        cal_stick = 1;
        cal_phase = 0;
        cal_notch = 0;
        cal_cand[0] = cal_cand[1] = -1;
        for (int i = 0; i < 6; i++) {
          lo6[i] = INT_MAX;
          hi6[i] = INT_MIN;
        }
      } else {
        cal[0] = tmp[0];
        cal[1] = tmp[1];
        have_cal = true;
        rebuild_maps();
        eng_save_cfg();
        cal_active = false;
      }
    }
  }
}

/* ---------- trigger calibration ---------- */
void eng_trig_begin(void) {
  trig_active = true;
  trig_phase = 0;
  for (int c = 0; c < ABS_CNT; c++) {
    t_lo[c] = INT_MAX;
    t_hi[c] = INT_MIN;
  }
}

bool eng_trig_active(void) { return trig_active; }

int eng_trig_phase(void) { return trig_phase; }

void eng_trig_cancel(void) { trig_active = false; }

void eng_trig_advance(void) {
  if (!trig_active)
    return;
  if (trig_phase == 0) {
    trig_phase = 1;
    return;
  }
  for (int c = 0; c < ABS_CNT; c++)
    if (is_analog_abs(c) && t_hi[c] - t_lo[c] >= 16) {
      trig_lo[c] = t_lo[c];
      trig_hi[c] = t_hi[c];
      trig_on[c] = true;
      have_trig = true;
    }
  eng_save_cfg();
  trig_active = false;
}

bool eng_has_trig(void) { return have_trig; }

bool eng_is_trig(int code) {
  return code >= 0 && code < ABS_CNT && trig_on[code];
}

int eng_trig_out(int code, int raw) { return trig_apply(code, raw); }

/* ---------- GameCube button map ---------- */
/* find the D-pad: a non-stick hat axis (tiny range, signed) */
static void detect_dpad(void) {
  dpad_x = dpad_y = -1;
  if (!dev)
    return;
  for (int c = 0; c < ABS_CNT; c++) {
    if (!libevdev_has_event_code(dev, EV_ABS, c) || is_stick[c])
      continue;
    int mn = libevdev_get_abs_minimum(dev, c);
    int mx = libevdev_get_abs_maximum(dev, c);
    if (mx - mn > 2 || mn >= 0)
      continue; /* not a hat */
    const char *nm = libevdev_event_code_get_name(EV_ABS, c);
    if (nm && strchr(nm, 'Y') && dpad_y < 0)
      dpad_y = c;
    else if (dpad_x < 0)
      dpad_x = c;
    else if (dpad_y < 0)
      dpad_y = c;
  }
}

static void btnmap_advance(void) {
  map_idx++;
  if (map_idx >= GC_N) {
    detect_dpad();
    have_btnmap = true;
    eng_save_cfg();
    map_active = false;
  }
}

void eng_btnmap_begin(void) {
  map_active = true;
  map_idx = 0;
  for (int i = 0; i < GC_N; i++)
    gc_btn[i] = -1;
}

bool eng_btnmap_active(void) { return map_active; }

int eng_btnmap_index(void) { return map_idx; }

int eng_btnmap_total(void) { return GC_N; }

const char *eng_btnmap_name(void) {
  return (map_idx >= 0 && map_idx < GC_N) ? GC_NAMES[map_idx] : "";
}

void eng_btnmap_skip(void) {
  if (!map_active)
    return;
  gc_btn[map_idx] = -1;
  btnmap_advance();
}

void eng_btnmap_cancel(void) { map_active = false; }

bool eng_has_btnmap(void) { return have_btnmap; }
const char *eng_gc_name(int i) {
  return (i >= 0 && i < GC_N) ? GC_NAMES[i] : "";
}

bool eng_gc_pressed(int i) {
  if (i < 0 || i >= GC_N)
    return false;
  int c = gc_btn[i];
  return c >= 0 && c < KEY_CNT && keyst[c];
}

bool eng_gc_mapped(int i) { return i >= 0 && i < GC_N && gc_btn[i] >= 0; }
int eng_gc_code(int i) { return (i >= 0 && i < GC_N) ? gc_btn[i] : -1; }

/* D-pad direction as -1/0/+1 on each axis (raw hat sign; up = -y) */
void eng_dpad(int *x, int *y) {
  if (dpad_x < 0 && dpad_y < 0)
    detect_dpad();
  int vx = (dpad_x >= 0) ? cur[dpad_x] : 0;
  int vy = (dpad_y >= 0) ? cur[dpad_y] : 0;
  *x = (vx > 0) - (vx < 0);
  *y = (vy > 0) - (vy < 0);
}
