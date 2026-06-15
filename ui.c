#include "raylib.h"
#include <stddef.h>
#include <stdlib.h>
#define RAYGUI_IMPLEMENTATION
#include "engine.h"
#include "livesplit.h"
#include "raygui.h"
#include "skin.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 1060
#define H 756

/* ---- palette (dark) ---------------------------------------------------- */
static const Color BG = {15, 17, 23, 255};       /* window background      */
static const Color PANEL = {25, 28, 38, 255};    /* card background       */
static const Color PANEL2 = {19, 21, 29, 255};   /* inset / plot bg       */
static const Color LINE = {42, 47, 61, 255};     /* borders / separators  */
static const Color GRID = {38, 43, 56, 255};     /* faint grid            */
static const Color TXT = {214, 220, 232, 255};   /* primary text          */
static const Color DIM = {158, 169, 189, 255};   /* secondary text        */
static const Color ACCENT = {91, 157, 255, 255}; /* blue accent           */
static const Color GOOD = {70, 214, 160, 255};   /* green                 */
static const Color BAD = {255, 93, 108, 255};    /* red                   */
static const Color WARN = {255, 196, 87, 255};   /* amber                 */

static Font FONT, FONTB;                  /* regular + semibold */
static char FONT_REG[512], FONT_BLD[512]; /* TTF paths, for hi-res PNG export */

/* ---- text helpers ------------------------------------------------------ */
static void txt(Font f, const char *s, float x, float y, float sz, Color c) {
  /* snap to whole pixels so glyph edges stay crisp */
  DrawTextEx(f, s, (Vector2){roundf(x), roundf(y)}, sz, sz / 16.0f, c);
}

/* on-screen width of s at size sz, matching txt()'s letter spacing */
static float tw(Font f, const char *s, float sz) {
  return MeasureTextEx(f, s, sz, sz / 16.0f).x;
}

/* ---- viewer helpers ---------------------------------------------------- */
static void viewer_path(char *b, size_t n) { eng_config_path(b, n, "viewer"); }

static void viewer_save(int bg, bool bl, bool sv, bool hp) {
  char p[600];
  viewer_path(p, sizeof p);
  FILE *f = fopen(p, "w");
  if (f) {
    fprintf(f, "%d %d %d %d\n", bg, bl ? 1 : 0, sv ? 1 : 0, hp ? 1 : 0);
    fclose(f);
  }
}

static void viewer_load(int *bg, bool *bl, bool *sv, bool *hp) {
  char p[600];
  viewer_path(p, sizeof p);
  FILE *f = fopen(p, "r");
  if (f) {
    /* v defaults on, h off; tolerates older 2/3-field files */
    int b = 0, d = 0, v = 1, h = 0;
    if (fscanf(f, "%d %d %d %d", &b, &d, &v, &h) >= 2) {
      *bg = ((b % 3) + 3) % 3;
      *bl = d != 0;
      *sv = v != 0;
      *hp = h != 0;
    }
    fclose(f);
  }
}

/* ---- separate viewer process (its own window) -------------------------- */
static void viewer_src_path(char *b, size_t n) {
  eng_config_path(b, n, "viewer_src");
}
/* the control process publishes which devnode the viewer should read: the
   virtual remap mirror while remapping, otherwise the physical controller */
static void publish_viewer_src(void) {
  const char *src = "";
  if (eng_remap_active()) {
    const char *d = eng_remap_devnode();
    if (d)
      src = d;
  } else {
    const char *d = eng_dev_path();
    if (d && strcmp(d, "(none)"))
      src = d;
  }
  static char last[300] = "\x01"; /* impossible value forces the first write */
  if (!strcmp(src, last))
    return;
  snprintf(last, sizeof last, "%s", src);
  char p[600];
  viewer_src_path(p, sizeof p);
  FILE *f = fopen(p, "w");
  if (f) {
    fprintf(f, "%s\n", src);
    fclose(f);
  }
}
static void read_viewer_src(char *out, size_t n) {
  out[0] = 0;
  char p[600];
  viewer_src_path(p, sizeof p);
  FILE *f = fopen(p, "r");
  if (f) {
    if (fgets(out, n, f))
      out[strcspn(out, "\r\n")] = 0;
    fclose(f);
  }
}
/* spawn a second instance of ourselves as an independent viewer window */
static void launch_viewer_window(void) {
  char exe[512];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0)
    return;
  exe[n] = 0;
  pid_t pid = fork();
  if (pid == 0) {
    execl(exe, exe, "--viewer", (char *)NULL);
    _exit(127);
  }
}

/* ---- run log (one finished speedrun per line) -------------------------- */
static void runs_path(char *b, size_t n) { eng_config_path(b, n, "runs.log"); }

/* unix-time \t game-time-ms \t button-presses */
static void append_run_log(long when, long game_ms, long presses) {
  char p[600];
  runs_path(p, sizeof p);
  FILE *f = fopen(p, "a");
  if (f) {
    fprintf(f, "%ld\t%ld\t%ld\n", when, game_ms, presses);
    fclose(f);
  }
}

// in-memory ring of finished runs, newest last
#define RUN_HIST 200
static void push_run(long *when, long *ms, long *pr, int *n, long w, long m,
                     long p) {
  if (*n == RUN_HIST) {
    // Full: drop the oldest, shift down by one
    memmove(when, when + 1, (RUN_HIST - 1) * sizeof(long));
    memmove(ms, ms + 1, (RUN_HIST - 1) * sizeof(long));
    memmove(pr, pr + 1, (RUN_HIST - 1) * sizeof(long));
    (*n)--;
  }
  when[*n] = w;
  ms[*n] = m;
  pr[*n] = p;
  (*n)++;
}

/* ms -> "M:SS.mmm" (or "H:MM:SS.mmm" past an hour) */
static const char *fmt_ms(long ms) {
  if (ms < 0)
    ms = 0;
  long s = ms / 1000, m = s / 60, h = m / 60;
  if (h > 0)
    return TextFormat("%ld:%02ld:%02ld.%03ld", h, m % 60, s % 60, ms % 1000);
  return TextFormat("%ld:%02ld.%03ld", m, s % 60, ms % 1000);
}

/* derived figures over a set of finished runs */
typedef struct {
  int n;
  long best, worst, sum, total_ms;
  double avg, median, avg_apm, best_apm;
} run_agg;

static run_agg compute_agg(const long *ms, const long *pr, int n) {
  run_agg a = {0};
  a.n = n;
  a.best = a.worst = -1;
  for (int i = 0; i < n; i++) {
    if (a.best < 0 || pr[i] < a.best)
      a.best = pr[i];
    if (a.worst < 0 || pr[i] > a.worst)
      a.worst = pr[i];
    a.sum += pr[i];
    a.total_ms += ms[i];
    double rapm = ms[i] ? pr[i] * 60000.0 / ms[i] : 0;
    if (rapm > a.best_apm)
      a.best_apm = rapm;
  }
  long tmp[256] = {0};
  int m = n < 256 ? n : 256;
  for (int i = 0; i < m; i++)
    tmp[i] = pr[i];
  for (int i = 1; i < m; i++) { /* insertion sort for the median */
    long k = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > k) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = k;
  }
  int mid = m / 2;
  a.avg = n ? (double)a.sum / n : 0;
  a.median = m ? (double)tmp[mid] : 0;
  a.avg_apm = a.total_ms ? a.sum * 60000.0 / a.total_ms : 0;
  if (a.best < 0)
    a.best = 0;
  if (a.worst < 0)
    a.worst = 0;
  return a;
}

/* write a full CSV report (summary comments + per-run rows) to a timestamped
   file; returns the path written via outpath */
static bool export_stats_csv(char *outpath, size_t opn, const long *rh_when,
                             const long *rh_ms, const long *rh_pr, int rh_n) {
  time_t now = time(NULL);
  struct tm *tv = localtime(&now);
  char ts[32] = "export";
  if (tv)
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", tv);
  eng_config_path(outpath, opn, "stats-%s.csv", ts);
  FILE *f = fopen(outpath, "w");
  if (!f)
    return false;

  run_agg a = compute_agg(rh_ms, rh_pr, rh_n);
  fprintf(f, "# gcc-notch input stats export\n");
  fprintf(f, "# generated,%ld\n", (long)now);
  fprintf(f, "# lifetime_button_presses,%ld\n", eng_stats_presses());
  fprintf(f, "# lifetime_input_events,%ld\n", eng_stats_events());
  fprintf(f,
          "# dpad_up,%ld\n# dpad_down,%ld\n# dpad_left,%ld\n# dpad_right,%ld\n",
          eng_stats_dpad_count(0), eng_stats_dpad_count(1),
          eng_stats_dpad_count(2), eng_stats_dpad_count(3));
  fprintf(f, "# runs,%d\n# best_presses,%ld\n# worst_presses,%ld\n", a.n,
          a.best, a.worst);
  fprintf(f, "# avg_presses,%.1f\n# median_presses,%.0f\n", a.avg, a.median);
  fprintf(f, "# total_run_time_ms,%ld\n# avg_apm,%.1f\n# best_apm,%.1f\n",
          a.total_ms, a.avg_apm, a.best_apm);
  if (eng_has_btnmap())
    for (int i = 0; i < eng_btnmap_total(); i++) {
      int code = eng_gc_code(i);
      fprintf(f, "# button_%s,%ld\n", eng_gc_name(i),
              code >= 0 ? eng_stats_key(code) : 0);
    }
  fprintf(f, "run,datetime,game_time,game_ms,presses,apm\n");
  for (int i = 0; i < rh_n; i++) {
    char dt[32] = "";
    time_t w = (time_t)rh_when[i];
    struct tm *rt = localtime(&w);
    if (rt)
      strftime(dt, sizeof dt, "%Y-%m-%d %H:%M:%S", rt);
    double rapm = rh_ms[i] ? rh_pr[i] * 60000.0 / rh_ms[i] : 0;
    fprintf(f, "%d,%s,%s,%ld,%ld,%.1f\n", i + 1, dt, fmt_ms(rh_ms[i]), rh_ms[i],
            rh_pr[i], rapm);
  }
  fclose(f);
  return true;
}

/* path for a single run's export, named by its timestamp */
static void run_export_path(char *b, size_t n, const char *ext, long when) {
  time_t w = (time_t)when;
  struct tm *tv = localtime(&w);
  char ts[32] = "run";
  if (tv)
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", tv);
  eng_config_path(b, n, "run-%s.%s", ts, ext);
}

/* one run -> CSV (single row, plus a per-button section when available) */
static bool export_run_csv(char *outpath, size_t opn, int idx1, long when,
                           long ms, long pr, const long *bykey, int nkey) {
  run_export_path(outpath, opn, "csv", when);
  FILE *f = fopen(outpath, "w");
  if (!f)
    return false;
  char dt[32] = "";
  time_t w = (time_t)when;
  struct tm *tv = localtime(&w);
  if (tv)
    strftime(dt, sizeof dt, "%Y-%m-%d %H:%M:%S", tv);
  double apm = ms ? pr * 60000.0 / ms : 0;
  fprintf(f, "run,datetime,game_time,game_ms,presses,apm\n");
  fprintf(f, "%d,%s,%s,%ld,%ld,%.1f\n", idx1, dt, fmt_ms(ms), ms, pr, apm);
  if (bykey) {
    fprintf(f, "\nbutton,presses\n");
    for (int i = 0; i < nkey; i++)
      fprintf(f, "%s,%ld\n", eng_gc_name(i), bykey[i]);
  }
  fclose(f);
  return true;
}

/* one run -> a shareable PNG result card. Must run OUTSIDE BeginDrawing().
   Rendered supersampled (S x) with fonts reloaded at a large base size so the
   text stays crisp at the higher resolution. */
static bool export_run_png(char *outpath, size_t opn, int idx1, long when,
                           long ms, long pr, const long *bykey, int nkey) {
  run_export_path(outpath, opn, "png", when);
  const float S = 3.0f; /* supersample factor */
  int IW = (int)(640 * S), IH = (int)((bykey ? 392 : 248) * S);

  /* crisp high-res fonts just for this image; fall back to the UI atlas */
  Font hf = FONT, hb = FONTB;
  bool fr = FONT_REG[0] && FileExists(FONT_REG);
  bool fb = FONT_BLD[0] && FileExists(FONT_BLD);
  if (fr) {
    hf = LoadFontEx(FONT_REG, 192, NULL, 0);
    GenTextureMipmaps(&hf.texture);
    SetTextureFilter(hf.texture, TEXTURE_FILTER_TRILINEAR);
  }
  if (fb) {
    hb = LoadFontEx(FONT_BLD, 192, NULL, 0);
    GenTextureMipmaps(&hb.texture);
    SetTextureFilter(hb.texture, TEXTURE_FILTER_TRILINEAR);
  }
#define TT(f, s, x, y, sz, c)                                                  \
  DrawTextEx(f, s, (Vector2){(x) * S, (y) * S}, (sz) * S, (sz) * S / 16.0f, c)

  RenderTexture2D rt = LoadRenderTexture(IW, IH);
  BeginTextureMode(rt);
  ClearBackground((Color){18, 20, 28, 255});
  DrawRectangle(0, 0, IW, (int)(4 * S), ACCENT);
  DrawRectangleLinesEx((Rectangle){0, 0, (float)IW, (float)IH}, S, LINE);

  char dt[32] = "";
  time_t w = (time_t)when;
  struct tm *tv = localtime(&w);
  if (tv)
    strftime(dt, sizeof dt, "%Y-%m-%d %H:%M", tv);
  double apm = ms ? pr * 60000.0 / ms : 0;

  TT(hb, TextFormat("Run #%d", idx1), 28, 24, 30, TXT);
  TT(hf, dt, 28, 62, 16, DIM);
  TT(hb, fmt_ms(ms), 28, 92, 56, GOOD);
  TT(hb, TextFormat("%ld", pr), 28, 168, 30, ACCENT);
  TT(hf, "presses", 28, 202, 16, DIM);
  TT(hb, TextFormat("%.0f", apm), 240, 168, 30, TXT);
  TT(hf, "apm", 240, 202, 16, DIM);

  if (bykey) {
    DrawRectangle((int)(28 * S), (int)(236 * S), IW - (int)(56 * S), (int)S,
                  LINE);
    for (int i = 0; i < nkey; i++) {
      int row = i / 4, col = i % 4;
      float x = 28 + col * 150.0f, y = 252 + row * 30.0f;
      TT(hb, eng_gc_name(i), x, y, 18, TXT);
      TT(hf, TextFormat("%ld", bykey[i]), x + 50, y, 18, DIM);
    }
  }
  TT(hf, "gcc-notch", 640 - 104, (bykey ? 392 : 248) - 26, 14, Fade(DIM, 0.6f));
  EndTextureMode();
#undef TT

  Image img = LoadImageFromTexture(rt.texture);
  ImageFlipVertical(&img); /* render textures read back upside-down */
  bool ok = ExportImage(img, outpath);
  UnloadImage(img);
  UnloadRenderTexture(rt);
  if (fr)
    UnloadFont(hf);
  if (fb)
    UnloadFont(hb);
  return ok;
}

/* ---- shape helpers ----------------------------------------------------- */
static void card(Rectangle r) {
  DrawRectangleRounded(r, 0.045f, 8, PANEL);
  DrawRectangleRoundedLinesEx(r, 0.045f, 8, 1.0f, LINE);
}

/* dim the screen and draw a centered modal card; returns its rect */
static Rectangle modal_box(float w, float h, float rnd, Color accent) {
  DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
  Rectangle box = {(W - w) / 2.0f, (H - h) / 2.0f, w, h};
  DrawRectangleRounded(box, rnd, 10, PANEL);
  DrawRectangleRoundedLinesEx(box, rnd, 10, 1.0f, Fade(accent, 0.6f));
  return box;
}

/* section heading: a small accent bar + label */
static void section_title(const char *s, float x, float y) {
  DrawRectangleRounded((Rectangle){x, y + 3, 3, 14}, 1.0f, 4, ACCENT);
  DrawTextEx(FONTB, s, (Vector2){roundf(x + 11), roundf(y)}, 17, 17 / 16.0f,
             TXT);
}

/* soft glowing dot: a halo behind a solid core */
static void glow_dot(Vector2 p, float r, Color c) {
  DrawCircleV(p, r * 3.2f, Fade(c, 0.10f));
  DrawCircleV(p, r * 2.0f, Fade(c, 0.18f));
  DrawCircleV(p, r, c);
  DrawCircleV(p, r * 0.45f, Fade(WHITE, 0.65f));
}
/* fill a fan triangle regardless of winding order */
static void tri2(Vector2 a, Vector2 b, Vector2 cc, Color col) {
  DrawTriangle(a, b, cc, col);
  DrawTriangle(a, cc, b, col);
}

static Vector2 rts(Rectangle a, double rx, double ry) {
  int lo = eng_axis_min(), hi = eng_axis_max();
  double t = hi - lo;
  if (t <= 0)
    t = 255;
  return (Vector2){a.x + (float)((rx - lo) / t) * a.width,
                   a.y + (float)((ry - lo) / t) * a.height};
}

static const char *shortname(const char *s) {
  if (!strncmp(s, "BTN_", 4) || !strncmp(s, "KEY_", 4) ||
      !strncmp(s, "ABS_", 4))
    return s + 4;
  return s;
}

/* join count names with ';' into buf for a raygui dropdown list */
static void dropdown_items(char *buf, size_t n, int count,
                           const char *(*name)(int)) {
  buf[0] = 0;
  for (int i = 0; i < count; i++) {
    if (i)
      strncat(buf, ";", n - strlen(buf) - 1);
    strncat(buf, name(i), n - strlen(buf) - 1);
  }
}

/* ---- per-stick input trail (ring buffer of recent raw/remapped points) - */
#define TRAIL_N 90
static int rxh[2][TRAIL_N], ryh[2][TRAIL_N], oxh[2][TRAIL_N], oyh[2][TRAIL_N];
static int thead[2], tcount[2];

static void trail_push(int s, int rx, int ry, int ox, int oy) {
  int h = thead[s];
  rxh[s][h] = rx, ryh[s][h] = ry, oxh[s][h] = ox, oyh[s][h] = oy;
  thead[s] = (h + 1) % TRAIL_N;
  if (tcount[s] < TRAIL_N)
    tcount[s]++;
}
static void trail_draw(Rectangle a, int s, bool cal) {
  int cnt = tcount[s];
  int base = (thead[s] - cnt + 2 * TRAIL_N) % TRAIL_N;
  for (int k = 1; k < cnt; k++) {
    int i0 = (base + k - 1) % TRAIL_N, i1 = (base + k) % TRAIL_N;
    float age = (float)k / cnt; /* 0 = oldest, 1 = newest */
    DrawLineEx(rts(a, rxh[s][i0], ryh[s][i0]), rts(a, rxh[s][i1], ryh[s][i1]),
               1.2f, Fade(BAD, 0.04f + 0.30f * age));
    if (cal)
      DrawLineEx(rts(a, oxh[s][i0], oyh[s][i0]), rts(a, oxh[s][i1], oyh[s][i1]),
                 1.2f, Fade(GOOD, 0.04f + 0.30f * age));
  }
}

static void draw_stick(Rectangle outer, const char *title, int stick) {
  card(outer);
  section_title(title, outer.x + 16, outer.y + 12);

  /* plot area inset below the title (leaves room for the diag slider) */
  Rectangle a = {outer.x + 16, outer.y + 40, outer.width - 32,
                 outer.height - 84};
  DrawRectangleRounded(a, 0.03f, 6, PANEL2);

  Vector2 ctr = rts(a, 128, 128);

  /* range circle */
  DrawCircleLinesV(ctr, a.width * 0.46f, GRID);

  /* center deadzone ring (a raw circle maps to an ellipse in plot space) */
  double dz = eng_get_deadzone();
  if (dz > 0)
    DrawEllipseLines((int)ctr.x, (int)ctr.y, (float)dz * a.width / 2.0f,
                     (float)dz * a.height / 2.0f, Fade(BAD, 0.45f));

  /* grid: quarter divisions */
  for (int i = 1; i < 4; i++) {
    float fx = a.x + a.width * i / 4.0f;
    float fy = a.y + a.height * i / 4.0f;
    DrawLineV((Vector2){fx, a.y}, (Vector2){fx, a.y + a.height}, GRID);
    DrawLineV((Vector2){a.x, fy}, (Vector2){a.x + a.width, fy}, GRID);
  }
  /* crosshair through center, a touch brighter */
  DrawLineV((Vector2){a.x, ctr.y}, (Vector2){a.x + a.width, ctr.y},
            Fade(DIM, 0.35f));
  DrawLineV((Vector2){ctr.x, a.y}, (Vector2){ctr.x, a.y + a.height},
            Fade(DIM, 0.35f));

  const eng_stick_cal *c = eng_cal(stick);
  if (eng_has_cal()) {
    /* ideal octagon: soft fill + crisp outline */
    Vector2 ideal[ENG_NOTCH];
    for (int i = 0; i < ENG_NOTCH; i++) {
      double wx, wy;
      eng_ideal_vec(stick, i, &wx, &wy);
      ideal[i] = rts(a, 128 + wx, 128 + wy);
    }
    for (int i = 0; i < ENG_NOTCH; i++)
      tri2(ctr, ideal[i], ideal[(i + 1) % ENG_NOTCH], Fade(GOOD, 0.06f));
    for (int i = 0; i < ENG_NOTCH; i++)
      DrawLineEx(ideal[i], ideal[(i + 1) % ENG_NOTCH], 1.5f, Fade(GOOD, 0.55f));

    /* measured notch polygon: blue outline + nodes */
    Vector2 meas[ENG_NOTCH];
    for (int i = 0; i < ENG_NOTCH; i++) {
      double vx, vy;
      eng_measured_vec(stick, i, &vx, &vy);
      meas[i] = rts(a, c->cx + vx, c->cy + vy);
    }
    for (int i = 0; i < ENG_NOTCH; i++)
      DrawLineEx(meas[i], meas[(i + 1) % ENG_NOTCH], 2.0f, Fade(ACCENT, 0.75f));
    for (int i = 0; i < ENG_NOTCH; i++) {
      DrawCircleV(meas[i], 4.5f, Fade(ACCENT, 0.22f));
      DrawCircleV(meas[i], 2.5f, ACCENT);
    }
  }

  int rx = eng_raw(c->ax), ry = eng_raw(c->ay);
  bool cal = eng_has_cal();
  int ox = rx, oy = ry;
  if (cal)
    eng_remap_point(stick, rx, ry, &ox, &oy);

  /* fading input trail (drawn under the live dots) */
  trail_push(stick, rx, ry, ox, oy);
  trail_draw(a, stick, cal);

  if (cal) {
    /* line from raw -> remapped to show the correction */
    DrawLineEx(rts(a, rx, ry), rts(a, ox, oy), 1.5f, Fade(GOOD, 0.35f));
    glow_dot(rts(a, ox, oy), 5.0f, GOOD);
  }
  glow_dot(rts(a, rx, ry), 5.0f, BAD);

  /* live axis values, bottom-left, on a backing so they read over the grid */
  const char *r1 = TextFormat("raw  %d, %d", rx, ry);
  const char *r2 = cal ? TextFormat("out  %d, %d", ox, oy) : "";
  float w1 = tw(FONT, r1, 13);
  float w2 = cal ? tw(FONT, r2, 13) : 0;
  float bw = (w1 > w2 ? w1 : w2) + 14, bh = cal ? 42 : 23;
  Rectangle rb = {a.x + 6, a.y + a.height - bh - 6, bw, bh};
  DrawRectangleRounded(rb, 0.28f, 6, Fade(PANEL2, 0.85f));
  txt(FONT, r1, rb.x + 7, rb.y + 4, 13, BAD);
  if (cal)
    txt(FONT, r2, rb.x + 7, rb.y + 21, 13, GOOD);

  /* per-stick diagonal slider under the plot */
  float dv = (float)eng_get_diag(stick);
  txt(FONT, "diag", a.x, a.y + a.height + 12, 13, DIM);
  Rectangle ds = {a.x + 44, a.y + a.height + 10, a.width - 44 - 44, 16};
  GuiSliderBar(ds, NULL, TextFormat("%.2f", dv), &dv, 0.30f, 1.0f);
  if (fabsf(dv - (float)eng_get_diag(stick)) > 1e-4f)
    eng_set_diag(stick, dv);
}

/* a small legend chip: colored dot + label */
static float chip(float x, float y, Color c, const char *label) {
  DrawCircleV((Vector2){x + 5, y + 7}, 5, c);
  txt(FONT, label, x + 16, y, 13, DIM);
  return x + 16 + tw(FONT, label, 13) + 22;
}

/* ---- headless daemon mode --------------------------------------------- */
static volatile sig_atomic_t daemon_run = 1;
static void on_signal(int sig) {
  (void)sig;
  daemon_run = 0;
}

static int run_daemon(const char *devpath) {
  eng_load_cfg();
  if (!eng_has_cal()) {
    fprintf(
        stderr,
        "gcc-notch: no calibration found; run the GUI to calibrate first\n");
    return 1;
  }
  eng_open(
      devpath); /* may be disconnected now; reconnect loop will pick it up */
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  eng_start_remap(); /* sets intent; resumes automatically once connected */
  fprintf(stderr, "gcc-notch: remapping in daemon mode (Ctrl-C to stop)\n");
  while (daemon_run) {
    eng_poll();
    usleep(1000);
  }
  eng_close();
  return 0;
}

/* ---- skinned stream viewer -------------------------------------------- */
static void draw_stream_view(int bg, bool show_hint) {
  /* the window is resized to the skin in the viewer, so draw to its live size
   */
  int ww = GetScreenWidth(), wh = GetScreenHeight();
  Color bgc = bg == 1   ? (Color){0, 177, 64, 255}
              : bg == 2 ? (Color){255, 0, 255, 255}
                        : (Color){8, 8, 10, 255};
  ClearBackground(bgc);

  if (skin_have()) {
    skin_draw(ww, wh);
    if (show_hint) {
      const char *au = skin_author(skin_current());
      txt(FONT,
          (au && au[0])
              ? TextFormat("skin: %s  ·  by %s", skin_name(skin_current()), au)
              : TextFormat("skin: %s", skin_name(skin_current())),
          24, 24, 14, Fade(WHITE, 0.6f));
      txt(FONT,
          "V exit  K skin  R reload  B background  F borderless  N values  H "
          "pin",
          24, wh - 30, 14, Fade(WHITE, 0.5f));
    }
  } else {
    const char *m1 = "No skins found.";
    const char *m2 =
        "Drop a skin folder (with skin.xml) into ~/.config/gcc-notch/skins/";
    txt(FONTB, m1, ww / 2.0f - tw(FONTB, m1, 22) / 2, wh / 2.0f - 30, 22, TXT);
    txt(FONT, m2, ww / 2.0f - tw(FONT, m2, 14) / 2, wh / 2.0f + 4, 14, DIM);
  }
}

/* force the window to the skin's native ("actual input viewer") size so the
   overlay is pixel-perfect with no scaling or borders; only shrink if the skin
   is larger than the monitor. Restore the editor size when there's no skin. */
static int g_winw = W, g_winh = H; /* the viewer's locked target size */
static void fit_window_to_skin(void) {
  int sw, sh;
  if (skin_have() && skin_size(&sw, &sh) && sw > 0 && sh > 0) {
    int mon = GetCurrentMonitor();
    int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
    float sc = 1.0f;
    if (mw > 0 && mh > 0)
      sc = fminf(1.0f, fminf((float)mw / sw, (float)mh / sh));
    g_winw = (int)(sw * sc + 0.5f);
    g_winh = (int)(sh * sc + 0.5f);
  } else {
    g_winw = W;
    g_winh = H;
  }
  SetWindowSize(g_winw, g_winh);
}

/* viewer hotkeys shared by the in-window stream view and the standalone viewer
 * process: cycle/reload skins, change background, toggle
 * values/hint/border,
 * persisting the viewer prefs on every change */
static void viewer_hotkeys(int *bg, bool *bl, bool *sv, bool *hp) {
  if (IsKeyPressed(KEY_K)) {
    skin_next();
    fit_window_to_skin();
  }
  if (IsKeyPressed(KEY_R)) {
    skin_reload();
    fit_window_to_skin();
  }
  if (IsKeyPressed(KEY_B)) {
    *bg = (*bg + 1) % 3;
    viewer_save(*bg, *bl, *sv, *hp);
  }
  if (IsKeyPressed(KEY_N)) {
    *sv = !*sv;
    skin_set_values(*sv);
    viewer_save(*bg, *bl, *sv, *hp);
  }
  if (IsKeyPressed(KEY_H)) {
    *hp = !*hp;
    viewer_save(*bg, *bl, *sv, *hp);
  }
  if (IsKeyPressed(KEY_F)) {
    *bl = !*bl;
    if (*bl)
      SetWindowState(FLAG_WINDOW_UNDECORATED);
    else
      ClearWindowState(FLAG_WINDOW_UNDECORATED);
    viewer_save(*bg, *bl, *sv, *hp);
  }
}

/* dedicated viewer process: no controls, follows the device the control
   process publishes (physical when idle, virtual remap mirror when remapping)
 */
static void run_viewer_loop(void) {
  SetWindowTitle("GCC Notch Viewer");
  skin_set_remap_display(false); /* show the source device's raw values */
  int bg = 0;
  bool bl = false, sv = true, hp = false;
  viewer_load(&bg, &bl, &sv, &hp);
  skin_set_values(sv);
  if (bl)
    SetWindowState(FLAG_WINDOW_UNDECORATED);
  fit_window_to_skin();

  char cur_src[300] = "";
  double last_chk = 0, enter = GetTime();
  while (!WindowShouldClose()) {
    /* hard-lock the window to the skin size: if the compositor (tiling, a stray
       drag, a resize glitch) changed it, snap straight back so OBS only ever
       sees the skin with no background bleed */
    if (GetScreenWidth() != g_winw || GetScreenHeight() != g_winh)
      SetWindowSize(g_winw, g_winh);

    if (GetTime() - last_chk > 0.25) { /* follow the published input device */
      last_chk = GetTime();
      char src[300];
      read_viewer_src(src, sizeof src);
      if (src[0] && strcmp(src, cur_src)) {
        eng_close();
        eng_open(src);
        snprintf(cur_src, sizeof cur_src, "%s", src);
      } else if (!src[0] && !eng_connected()) {
        eng_open(NULL);
      }
    }
    Vector2 md = GetMouseDelta();
    if (md.x != 0.0f || md.y != 0.0f)
      enter = GetTime();
    viewer_hotkeys(&bg, &bl, &sv, &hp);
    if (IsKeyPressed(KEY_V) || IsKeyPressed(KEY_ESCAPE))
      break;
    BeginDrawing();
    draw_stream_view(bg, hp || GetTime() - enter < 4.0);
    EndDrawing();
  }
}

int main(int argc, char **argv) {
  const char *devpath = NULL;
  bool daemon_mode = false, auto_remap = false, viewer_proc = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--daemon") || !strcmp(argv[i], "-d"))
      daemon_mode = true;
    else if (!strcmp(argv[i], "--remap"))
      auto_remap = true;
    else if (!strcmp(argv[i], "--viewer"))
      viewer_proc = true; /* run as a standalone viewer window */
    else
      devpath = argv[i];
  }
  if (daemon_mode)
    return run_daemon(devpath);

  /* no HIGHDPI: displays here run at scale 1.0 (render == screen), so it buys
     nothing and triggers a resize feedback fight with the compositor.
     The editor scales to fit and is resizable; the viewer is locked to the
     skin's exact size (non-resizable) so OBS never sees background around it.
   */
  unsigned cfgflags = FLAG_VSYNC_HINT;
  if (!viewer_proc)
    cfgflags |= FLAG_WINDOW_RESIZABLE;
  SetConfigFlags(cfgflags);
  /* the title doubles as the Wayland app_id (class) and the class is fixed at
     creation, so name the viewer process distinctly up front -- that lets a
     Hyprland rule match it by class and float it immediately */
  InitWindow(W, H, viewer_proc ? "GCC Notch Viewer" : "GCC Notch Remapper");
  SetTargetFPS(240);
  SetExitKey(KEY_NULL); /* we handle Esc ourselves (modal cancel / quit) */

  /* crisp TTF; fall back to the built-in font if unavailable. Medium body /
     Bold headings read better than Regular/SemiBold at small sizes, and since
     the font is monospace the heavier weight keeps identical glyph widths (no
     layout shift). */
  const char *reg = "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Medium.ttf";
  const char *bld = "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Bold.ttf";
  bool gotreg = FileExists(reg), gotbld = FileExists(bld);
  if (gotreg)
    snprintf(FONT_REG, sizeof FONT_REG, "%s", reg);
  if (gotbld)
    snprintf(FONT_BLD, sizeof FONT_BLD, "%s", bld);
  /* The window renders 1:1 (no display scaling) and no UI text exceeds ~26px,
     so a 64px mipmapped/trilinear atlas was minifying every glyph ~4x through
     blurry mip levels -- the "bloom". A tight 36px atlas with plain bilinear
     keeps glyphs near native size and crisp. */
  FONT = gotreg ? LoadFontEx(reg, 36, NULL, 0) : GetFontDefault();
  FONTB = gotbld ? LoadFontEx(bld, 36, NULL, 0) : FONT;
  if (gotreg)
    SetTextureFilter(FONT.texture, TEXTURE_FILTER_BILINEAR);
  if (gotbld)
    SetTextureFilter(FONTB.texture, TEXTURE_FILTER_BILINEAR);

  /* raygui dark theme */
  GuiSetFont(FONT);
  skin_set_font(FONT);
  GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
  GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
  GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(BG));
  GuiSetStyle(DEFAULT, LINE_COLOR, ColorToInt(LINE));
  GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(LINE));
  GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt((Color){35, 39, 52, 255}));
  GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(TXT));
  GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(ACCENT));
  GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,
              ColorToInt((Color){44, 50, 66, 255}));
  GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
  GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(BG));

  eng_load_cfg();
  eng_open(devpath);
  eng_io_start(); /* poll/remap on a background thread, immune to render stalls
                   */
  skin_load_all();

  /* standalone viewer process: just the overlay, driven by the control app */
  if (viewer_proc) {
    run_viewer_loop();
    eng_io_stop();
    eng_close();
    CloseWindow();
    return 0;
  }

  signal(SIGCHLD, SIG_IGN); /* reap spawned viewer windows automatically */
  MaximizeWindow();         /* fill the screen; Hyprland tiles it fullscreen */
  eng_stats_load();         /* control process owns the saved stats file */
  ls_start();               /* probe dusklight's LiveSplit timer commands */
  if (auto_remap)
    eng_start_remap();

  /* profile dropdown + modal state (persist across frames) */
  int dd_idx = 0;
  bool dd_edit = false;
  int sk_idx = 0;
  bool sk_edit = false;
  bool name_modal = false, confirm_reset = false;
  bool show_stats = false, confirm_stats_reset = false;
  char name_buf[64] = "";

  /* run tracking, driven by dusklight's LiveSplit commands */
  long rh_when[RUN_HIST], rh_ms[RUN_HIST], rh_pr[RUN_HIST];
  int rh_n = 0;                        /* runs held, newest at rh_n-1 */
  long ls_seen_start = ls_start_seq(); /* ignore any pre-existing run */
  long ls_seen_end = ls_end_seq();
  long run_base = 0;             /* eng_stats_presses() snapshot at run start */
  bool run_live = false;         /* a run is currently in progress */
  long last_pr = 0, last_ms = 0; /* most recent finished run, for display */

  /* per-GameCube-button snapshot, for the per-run breakdown */
  long run_base_key[16] = {0}; /* per-button presses at run start */
  long last_key[16] = {0};     /* per-button presses of the last run */

  /* rolling-window APM and its per-run peak */
#define APM_N 512
#define APM_WINDOW 3.0 /* seconds */
  double apm_t[APM_N] = {0};
  long apm_c[APM_N] = {0};
  int apm_head = 0, apm_cnt = 0;
  long apm_prev = eng_stats_presses(); /* presses seen last frame */
  double cur_apm = 0, run_peak_apm = 0, last_peak_apm = 0;

  /* export feedback toast */
  char export_msg[300] = "";
  double export_msg_t = 0;

  /* per-run export: which run is selected, and a deferred PNG request (the
     render-texture work must happen outside BeginDrawing) */
  int sel_run = -1; /* index into rh_*, -1 = most recent */
  bool png_pending = false;
  int png_idx = 0;
  long png_when = 0, png_ms = 0, png_pr = 0, png_key[16] = {0};
  bool png_haskey = false;

  /* preload recent runs from the log so history survives restarts */
  {
    char p[600];
    runs_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
      long w, g, pr;
      while (fscanf(f, "%ld %ld %ld", &w, &g, &pr) == 3) {
        push_run(rh_when, rh_ms, rh_pr, &rh_n, w, g, pr);
      }
      fclose(f);
      if (rh_n > 0) {
        last_pr = rh_pr[rh_n - 1];
        last_ms = rh_ms[rh_n - 1];
      }
    }
  }

  /* stream-view state */
  bool stream_view = false, borderless = false;
  int stream_bg = 0;       /* 0 = black, 1 = chroma green, 2 = chroma magenta */
  bool show_values = true; /* numeric stick readout overlay (toggle: N) */
  bool hint_pin = false;   /* H keeps the keybind hints on screen */
  bool was_stream = false; /* tracks viewer enter/exit to resize the window */
  viewer_load(&stream_bg, &borderless, &show_values, &hint_pin);
  skin_set_values(show_values);
  if (borderless)
    SetWindowState(FLAG_WINDOW_UNDECORATED);
  double stream_enter = GetTime();

  while (!WindowShouldClose()) {
    publish_viewer_src(); /* keep any viewer window pointed at the right device
                           */

    /* track LiveSplit run edges: snapshot presses at start, bank them at end */
    {
      long ss = ls_start_seq();
      if (ss != ls_seen_start) {
        ls_seen_start = ss;
        run_base = eng_stats_presses();
        run_live = true;
        run_peak_apm = 0;
        for (int i = 0; i < eng_btnmap_total() && i < 16; i++) {
          int code = eng_gc_code(i);
          run_base_key[i] = code >= 0 ? eng_stats_key(code) : 0;
        }
      }
      long es = ls_end_seq();
      if (es != ls_seen_end) {
        ls_seen_end = es;
        if (run_live) {
          run_live = false;
          last_pr = eng_stats_presses() - run_base;
          last_ms = ls_final_ms();
          last_peak_apm = run_peak_apm;
          for (int i = 0; i < eng_btnmap_total() && i < 16; i++) {
            int code = eng_gc_code(i);
            last_key[i] =
                (code >= 0 ? eng_stats_key(code) : 0) - run_base_key[i];
          }
          long now = time(NULL);
          append_run_log(now, last_ms, last_pr);
          push_run(rh_when, rh_ms, rh_pr, &rh_n, now, last_ms, last_pr);
        }
      }
    }

    /* rolling-window APM: bin each frame's press burst, sum the trailing
       window, and remember the peak reached during the current run */
    {
      long pnow = eng_stats_presses();
      long d = pnow - apm_prev;
      apm_prev = pnow;
      double t = GetTime();
      if (d > 0) {
        apm_t[apm_head] = t;
        apm_c[apm_head] = d;
        apm_head = (apm_head + 1) % APM_N;
        if (apm_cnt < APM_N)
          apm_cnt++;
      }
      long wc = 0;
      for (int k = 0; k < apm_cnt; k++) {
        int idx = (apm_head - 1 - k + APM_N) % APM_N;
        if (t - apm_t[idx] > APM_WINDOW)
          break; /* samples are time-ordered, newest first */
        wc += apm_c[idx];
      }
      cur_apm = wc * 60.0 / APM_WINDOW;
      if (run_live && cur_apm > run_peak_apm)
        run_peak_apm = cur_apm;
    }

    /* V toggles the stream viewer in either mode */
    if (IsKeyPressed(KEY_V)) {
      stream_view = !stream_view;
      if (stream_view)
        stream_enter = GetTime();
    }

    if (stream_view) {
      /* any mouse movement re-shows the hint bar for a few seconds */
      Vector2 md = GetMouseDelta();
      if (md.x != 0.0f || md.y != 0.0f)
        stream_enter = GetTime();
      viewer_hotkeys(&stream_bg, &borderless, &show_values, &hint_pin);
      if (IsKeyPressed(KEY_ESCAPE))
        stream_view = false;
    } else {
      /* keyboard shortcuts (suppressed while a modal owns the screen) */
      bool modal = eng_cal_active() || eng_trig_active() ||
                   eng_btnmap_active() || name_modal || confirm_reset ||
                   show_stats;
      if (!modal) {
        if (IsKeyPressed(KEY_SPACE)) {
          if (eng_remap_active())
            eng_stop_remap();
          else
            eng_start_remap();
        }
        if (IsKeyPressed(KEY_C))
          eng_cal_begin();
        if (IsKeyPressed(KEY_T))
          eng_trig_begin();
        if (IsKeyPressed(KEY_R))
          eng_load_cfg();
        if (IsKeyPressed(KEY_S))
          show_stats = true;
      }
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (eng_cal_active())
          eng_cal_cancel();
        else if (eng_trig_active())
          eng_trig_cancel();
        else if (eng_btnmap_active())
          eng_btnmap_cancel();
        else if (name_modal)
          name_modal = false;
        else if (confirm_reset)
          confirm_reset = false;
        else if (confirm_stats_reset)
          confirm_stats_reset = false;
        else if (show_stats)
          show_stats = false;
        else
          break; /* quit */
      }
    }

    /* on entering/leaving the viewer, size the window to the skin (no borders
       for OBS) or back to filling the screen */
    if (stream_view != was_stream) {
      if (stream_view)
        fit_window_to_skin();
      else
        MaximizeWindow();
      was_stream = stream_view;
    }

    /* scale-to-fit: the editor is laid out on a fixed W x H design canvas and
       scaled to fill the (maximized) window; the stream view draws natively.
       Map the mouse back into design space so raygui hit-tests line up. */
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    float uiscale = fminf((float)sw / W, (float)sh / H);
    float uiox = (sw - W * uiscale) / 2.0f, uioy = (sh - H * uiscale) / 2.0f;
    if (stream_view) {
      SetMouseOffset(0, 0);
      SetMouseScale(1.0f, 1.0f);
    } else {
      SetMouseOffset((int)(-uiox), (int)(-uioy));
      SetMouseScale(1.0f / uiscale, 1.0f / uiscale);
    }

    /* stream view replaces the whole window */
    if (stream_view) {
      BeginDrawing();
      draw_stream_view(stream_bg, hint_pin || GetTime() - stream_enter < 4.0);
      EndDrawing();
      continue;
    }

    BeginDrawing();
    ClearBackground(BG); /* fills the letterbox bars around the design canvas */
    Camera2D uicam = {.offset = {uiox, uioy},
                      .target = {0, 0},
                      .rotation = 0,
                      .zoom = uiscale};
    BeginMode2D(uicam);

    /* ---- header -------------------------------------------------------- */
    DrawRectangle(0, 0, W, 64, PANEL);
    DrawLine(0, 64, W, 64, LINE);
    txt(FONTB, "GCC Notch Remapper", 28, 14, 24, TXT);
    txt(FONT, "GameCube controller calibration & notch remapping", 28, 42, 13,
        DIM);

    /* fps / event-rate readout, bottom-right (recomputed twice a second) */
    static double rate_t = 0;
    static long rate_ev = 0;
    static int evrate = 0;
    double now = GetTime();
    if (now - rate_t >= 0.5) {
      long ec = eng_event_count();
      evrate = (int)((ec - rate_ev) / (now - rate_t));
      rate_ev = ec;
      rate_t = now;
    }

    /* persist stats periodically, but only when something actually changed */
    static double stats_save_t = 0;
    static long stats_save_mark = -1;
    if (now - stats_save_t > 10.0) {
      long mark = eng_stats_presses() + eng_stats_events();
      if (mark != stats_save_mark) {
        eng_stats_save();
        stats_save_mark = mark;
      }
      stats_save_t = now;
    }
    const char *perf = TextFormat("%d fps   %d ev/s", GetFPS(), evrate);
    float pwf = tw(FONT, perf, 13);
    txt(FONT, perf, W - 28 - pwf, H - 22, 13, DIM);

    /* status pill, top-right */
    bool conn = eng_connected();

    /* drift watch: nudge toward recalibration once a calibrated stick has worn
   past its calibration. 8% under-reach or 6 axis-units of center drift.
   The estimate barely moves frame-to-frame, so recompute it a couple of times
   a second rather than scanning every notch on every frame. */
    static char drift_msg[96] = {0};
    static bool drifting = false;
    static double drift_t = -1;
    if (now - drift_t >= 0.5) {
      drift_t = now;
      drift_msg[0] = 0;
      drifting = false;
      if (conn && eng_has_cal()) {
        for (int s = 0; s < 2 && !drifting; s++) {
          eng_drift_info d;
          eng_drift(s, &d);
          if (!d.valid)
            continue;
          const char *sn = s == 0 ? "Control" : "C";
          if (d.worst_notch >= 0 && d.worst_reach_dev >= 0.08) {
            snprintf(drift_msg, sizeof drift_msg,
                     "%s stick: %s notch reach -%.0f%%  (recalibrate)", sn,
                     eng_notch_name(s, d.worst_notch),
                     d.worst_reach_dev * 100.0);
            drifting = true;
          } else if (d.center_dev >= 6.0) {
            snprintf(drift_msg, sizeof drift_msg,
                     "%s stick: center drift %.0f  (recalibrate)", sn,
                     d.center_dev);
            drifting = true;
          }
        }
      }
    }
    Color sc = !conn ? BAD : drifting ? WARN : eng_has_cal() ? GOOD : WARN;
    const char *st = !conn           ? "disconnected"
                     : drifting      ? "drift"
                     : eng_has_cal() ? "ready"
                                     : "not calibrated";

    float pw = tw(FONT, st, 14) + 44;
    Rectangle pill = {W - 28 - pw, 18, pw, 28};
    DrawRectangleRounded(pill, 1.0f, 12, PANEL2);
    DrawRectangleRoundedLinesEx(pill, 1.0f, 12, 1.0f, Fade(sc, 0.5f));
    DrawCircleV((Vector2){pill.x + 18, pill.y + 14}, 5, sc);
    txt(FONT, st, pill.x + 30, pill.y + 6, 14, TXT);

    /* REMAP ACTIVE badge (pulsing), left of the status pill */
    if (eng_remap_active()) {
      const char *bt = "REMAP ACTIVE";
      float bw = tw(FONT, bt, 13) + 36;
      Rectangle rb = {pill.x - 12 - bw, 18, bw, 28};
      DrawRectangleRounded(rb, 1.0f, 12, Fade(GOOD, 0.15f));
      DrawRectangleRoundedLinesEx(rb, 1.0f, 12, 1.0f, Fade(GOOD, 0.7f));
      float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
      DrawCircleV((Vector2){rb.x + 18, rb.y + 14}, 5,
                  Fade(GOOD, 0.35f + 0.65f * pulse));
      txt(FONT, bt, rb.x + 30, rb.y + 6, 13, GOOD);
    }

    /* ---- stick plots --------------------------------------------------- */
    draw_stick((Rectangle){28, 84, 312, 340}, "Control Stick", 0);
    draw_stick((Rectangle){352, 84, 312, 340}, "C-Stick", 1);

    /* legend */
    float lx = 28, ly = 434;
    lx = chip(lx, ly, BAD, "raw");
    lx = chip(lx, ly, GOOD, "remapped");
    lx = chip(lx, ly, ACCENT, "measured notches");
    lx = chip(lx, ly, Fade(GOOD, 0.7f), "ideal octagon");
    (void)lx;

    if (drifting)
      txt(FONT, drift_msg, 28, 448, 12, WARN);

    /* ---- right column: buttons & axes ---------------------------------- */
    Rectangle bpanel = {684, 84, W - 684 - 28, 300};
    card(bpanel);
    int colw = (int)(bpanel.width - 32) / 2;
    int bcx = (int)bpanel.x + 16, bcy = (int)bpanel.y + 44, col = 0;
    if (eng_has_btnmap()) {
      /* show the mapped GameCube buttons by name, lit from the stored map */
      int total = eng_btnmap_total();
      for (int i = 0; i < total; i++) {
        bool mapped = eng_gc_mapped(i), on = eng_gc_pressed(i);
        Rectangle led = {(float)bcx, (float)bcy, 14, 14};
        DrawRectangleRounded(led, 0.35f, 4, on ? GOOD : PANEL2);
        if (!on)
          DrawRectangleRoundedLinesEx(led, 0.35f, 4, 1.0f, LINE);
        txt(FONT,
            mapped ? TextFormat("%-6s %s", eng_gc_name(i),
                                shortname(eng_code_name_key(eng_gc_code(i))))
                   : TextFormat("%s  (unmapped)", eng_gc_name(i)),
            bcx + 22, bcy - 1, 13, on ? TXT : (mapped ? DIM : Fade(DIM, 0.5f)));
        bcy += 22;
        if (bcy > bpanel.y + bpanel.height - 24) {
          col++;
          bcy = (int)bpanel.y + 44;
          bcx = (int)bpanel.x + 16 + col * colw;
        }
      }
    } else {
      int codes[64];
      int n = eng_list_keys(codes, 64);
      for (int i = 0; i < n; i++) {
        bool on = eng_key(codes[i]);
        Rectangle led = {(float)bcx, (float)bcy, 14, 14};
        DrawRectangleRounded(led, 0.35f, 4, on ? GOOD : PANEL2);
        if (!on)
          DrawRectangleRoundedLinesEx(led, 0.35f, 4, 1.0f, LINE);
        txt(FONT, shortname(eng_code_name_key(codes[i])), bcx + 22, bcy - 1, 13,
            on ? TXT : DIM);
        bcy += 22;
        if (bcy > bpanel.y + bpanel.height - 24) {
          col++;
          bcy = (int)bpanel.y + 44;
          bcx = (int)bpanel.x + 16 + col * colw;
        }
      }
    }

    Rectangle apanel = {684, 400, W - 684 - 28, 266};
    card(apanel);
    section_title("Axes / Triggers", apanel.x + 16, apanel.y + 12);
    int ac[16];
    int m = eng_list_extra_abs(ac, 16);
    float ty = apanel.y + 44;
    for (int k = 0; k < m; k++) {
      int code = ac[k], v = eng_raw(code), lo = eng_abs_min(code),
          hi = eng_abs_max(code);
      bool tg = eng_is_trig(code);
      int outv = eng_trig_out(code, v);
      float fr = (hi > lo) ? (float)(v - lo) / (hi - lo) : 0;    /* raw */
      float fo = (hi > lo) ? (float)(outv - lo) / (hi - lo) : 0; /* remapped */
      txt(FONT, shortname(eng_code_name_abs(code)), apanel.x + 16, ty, 13, DIM);
      Rectangle bar = {apanel.x + 96, ty + 1, apanel.width - 96 - 16 - 50, 12};
      DrawRectangleRounded(bar, 1.0f, 6, PANEL2);
      float shown = tg ? fo : fr;
      if (shown > 0.001f) {
        Rectangle fill = {bar.x, bar.y, bar.width * shown, bar.height};
        DrawRectangleRounded(fill, 1.0f, 6, tg ? GOOD : ACCENT);
      }
      if (tg) {
        /* tick marking the actual raw (pre-rescale) position */
        float mx = bar.x + bar.width * fr;
        DrawLineEx((Vector2){mx, bar.y - 3},
                   (Vector2){mx, bar.y + bar.height + 3}, 1.5f,
                   Fade(DIM, 0.9f));
      }
      txt(FONT, TextFormat("%d%%", (int)(shown * 100 + 0.5f)),
          bar.x + bar.width + 8, ty, 13, tg ? GOOD : DIM);
      ty += 24;
    }
    if (!eng_has_trig())
      txt(FONT, "triggers not calibrated", apanel.x + 16, ty, 12, DIM);
    /* trigger bottom-deadzone slider */
    float tz = (float)eng_get_trig_dz();
    txt(FONT, "trig dz", apanel.x + 16, apanel.y + apanel.height - 32, 13, DIM);
    Rectangle tzs = {apanel.x + 96, apanel.y + apanel.height - 34,
                     apanel.width - 96 - 16 - 50, 16};
    GuiSliderBar(tzs, NULL, TextFormat("%.2f", tz), &tz, 0.0f, 0.40f);
    if (fabsf(tz - (float)eng_get_trig_dz()) > 1e-4f)
      eng_set_trig_dz(tz);

    /* ---- controls panel ------------------------------------------------ */
    Rectangle cp = {28, 462, 636, 204};
    card(cp);
    section_title("Controls", cp.x + 16, cp.y + 12);

    float row1 = cp.y + 44;
    if (GuiButton((Rectangle){cp.x + 16, row1, 138, 34},
                  eng_remap_active() ? "Stop Remap" : "Start Remap")) {
      if (eng_remap_active())
        eng_stop_remap();
      else
        eng_start_remap();
    }
    if (GuiButton((Rectangle){cp.x + 160, row1, 140, 34}, "Calibrate Sticks"))
      eng_cal_begin();
    if (GuiButton((Rectangle){cp.x + 306, row1, 150, 34}, "Calibrate Triggers"))
      eng_trig_begin();
    if (GuiButton((Rectangle){cp.x + 462, row1, 142, 34}, "Map Buttons"))
      eng_btnmap_begin();

    float row2 = cp.y + 88;
    float dzc = (float)eng_get_deadzone();
    GuiSliderBar((Rectangle){cp.x + 96, row2, 150, 30}, "Deadzone",
                 TextFormat("%.2f", dzc), &dzc, 0.0f, 0.30f);
    if (fabsf(dzc - (float)eng_get_deadzone()) > 1e-4f)
      eng_set_deadzone(dzc);
    if (GuiButton((Rectangle){cp.x + 290, row2, 104, 30}, "Reload cfg"))
      eng_load_cfg();
    if (GuiButton((Rectangle){cp.x + 406, row2, cp.width - 422, 30},
                  TextFormat("Port:  %s", eng_dev_path())))
      eng_dev_next();

    /* row3: profile management (dropdown itself is drawn later for z-order) */
    float row3 = cp.y + 124;
    if (GuiButton((Rectangle){cp.x + 196, row3, 64, 28}, "New")) {
      name_buf[0] = 0;
      name_modal = true;
    }
    if (GuiButton((Rectangle){cp.x + 268, row3, 76, 28}, "Delete"))
      eng_profile_delete(eng_profile_current());
    if (GuiButton((Rectangle){cp.x + 352, row3, 130, 28}, "Stats"))
      show_stats = true;
    if (GuiButton((Rectangle){cp.x + 490, row3, 130, 28}, "Reset Cal."))
      confirm_reset = true;

    /* profile dropdown, drawn last so its open list overlays the row above */
    int pc = eng_profile_count();
    if (pc > 0) {
      char items[1024];
      dropdown_items(items, sizeof items, pc, eng_profile_name);
      int curidx = 0;
      for (int i = 0; i < pc; i++)
        if (!strcmp(eng_profile_name(i), eng_profile_current()))
          curidx = i;
      if (!dd_edit)
        dd_idx = curidx;
      if (GuiDropdownBox((Rectangle){cp.x + 16, row3, 170, 28}, items, &dd_idx,
                         dd_edit)) {
        if (dd_edit) {
          dd_edit = false;
          eng_profile_select(eng_profile_name(dd_idx));
        } else {
          dd_edit = true;
        }
      }
    } else {
      txt(FONT, "(no profiles yet)", cp.x + 16, row3 + 6, 13, DIM);
    }

    /* row4: stream viewer + skin selector */
    float row4 = cp.y + 168;
    if (GuiButton((Rectangle){cp.x + 16, row4, 230, 30}, "Open Viewer Window"))
      launch_viewer_window(); /* separate process; V still opens it in-window */
    txt(FONT, "Skin", cp.x + 258, row4 + 8, 13, DIM);
    /* dropdown drawn after everything else so its open list overlays cleanly */
    int skc = skin_count();
    if (skc > 0) {
      char items[1024];
      dropdown_items(items, sizeof items, skc, skin_name);
      if (!sk_edit)
        sk_idx = skin_current() < 0 ? 0 : skin_current();
      if (GuiDropdownBox((Rectangle){cp.x + 300, row4, cp.width - 316, 30},
                         items, &sk_idx, sk_edit)) {
        if (sk_edit) {
          sk_edit = false;
          skin_select(sk_idx);
        } else
          sk_edit = true;
      }
    } else {
      txt(FONT, "(none installed)", cp.x + 300, row4 + 8, 13, DIM);
    }

    /* ---- calibration overlay ------------------------------------------- */
    if (eng_cal_active()) {
      Rectangle box = modal_box(500, 240, 0.06f, ACCENT);

      const char *sn = eng_cal_stick() == 0 ? "Control Stick" : "C-Stick";
      int ph = eng_cal_phase();
      txt(FONTB, "Calibration", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, sn, box.x + 28, box.y + 52, 14, ACCENT);

      const char *msg;
      char buf[160];
      if (ph == 0)
        msg = "Spin the stick fully around its gate, then press Next.";
      else if (ph == 1)
        msg = "Push and hold the stick fully RIGHT, then press Next.";
      else if (ph == 2)
        msg = "Let the stick rest at center, then press Capture.";
      else {
        snprintf(buf, sizeof buf, "Hold notch %d / 8, then press Capture.",
                 eng_cal_notch() + 1);
        msg = buf;
      }
      txt(FONT, msg, box.x + 28, box.y + 92, 16, TXT);

      /* step dots */
      for (int i = 0; i < 4; i++)
        DrawCircleV((Vector2){box.x + 28 + i * 16, box.y + 128}, 4,
                    i <= (ph > 3 ? 3 : ph) ? ACCENT : LINE);

      const eng_stick_cal *c = eng_cal(eng_cal_stick());
      txt(FONT, TextFormat("live  (%d, %d)", eng_raw(c->ax), eng_raw(c->ay)),
          box.x + 28, box.y + 150, 14, DIM);

      if (GuiButton((Rectangle){box.x + 28, box.y + 182, 150, 38},
                    ph <= 1 ? "Next" : "Capture"))
        eng_cal_advance();
      if (GuiButton((Rectangle){box.x + 322, box.y + 182, 150, 38}, "Cancel"))
        eng_cal_cancel();
    }

    /* ---- trigger calibration overlay ----------------------------------- */
    if (eng_trig_active()) {
      Rectangle box = modal_box(500, 260, 0.06f, ACCENT);

      int ph = eng_trig_phase();
      txt(FONTB, "Trigger Calibration", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, "Calibrates every analog L/R trigger", box.x + 28, box.y + 52,
          14, ACCENT);

      const char *msg = ph == 0 ? "Let both triggers rest (fully released)."
                                : "Now squeeze both triggers all the way down.";
      txt(FONT, msg, box.x + 28, box.y + 86, 16, TXT);

      for (int i = 0; i < 2; i++)
        DrawCircleV((Vector2){box.x + 28 + i * 16, box.y + 118}, 4,
                    i <= ph ? ACCENT : LINE);

      /* live readout of each analog axis */
      int ac2[16];
      int m2 = eng_list_extra_abs(ac2, 16);
      float ay = box.y + 138;
      for (int k = 0; k < m2; k++) {
        int code = ac2[k], lo = eng_abs_min(code), hi = eng_abs_max(code);
        if (hi - lo < 16)
          continue;
        float f = (float)(eng_raw(code) - lo) / (hi - lo);
        txt(FONT, shortname(eng_code_name_abs(code)), box.x + 28, ay, 13, DIM);
        Rectangle bar = {box.x + 150, ay + 1, 220, 10};
        DrawRectangleRounded(bar, 1.0f, 6, PANEL2);
        if (f > 0.001f)
          DrawRectangleRounded(
              (Rectangle){bar.x, bar.y, bar.width * f, bar.height}, 1.0f, 6,
              ACCENT);
        ay += 20;
        if (ay > box.y + 188)
          break;
      }

      if (GuiButton((Rectangle){box.x + 28, box.y + 202, 150, 38},
                    ph == 0 ? "Next" : "Capture"))
        eng_trig_advance();
      if (GuiButton((Rectangle){box.x + 322, box.y + 202, 150, 38}, "Cancel"))
        eng_trig_cancel();
    }

    /* ---- button-mapping wizard ----------------------------------------- */
    if (eng_btnmap_active()) {
      Rectangle box = modal_box(460, 220, 0.06f, ACCENT);

      txt(FONTB, "Map Buttons", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT,
          TextFormat("Step %d / %d", eng_btnmap_index() + 1,
                     eng_btnmap_total()),
          box.x + 28, box.y + 54, 14, DIM);

      const char *gcn = eng_btnmap_name();
      txt(FONT, "Press your", box.x + 28, box.y + 92, 16, TXT);
      txt(FONTB, gcn, box.x + 150, box.y + 88, 26, ACCENT);
      txt(FONT, "button", box.x + 150 + tw(FONTB, gcn, 26) + 12, box.y + 92, 16,
          TXT);
      txt(FONT, "(or Skip if your controller lacks it)", box.x + 28,
          box.y + 124, 13, DIM);

      /* progress dots */
      for (int i = 0; i < eng_btnmap_total(); i++)
        DrawCircleV((Vector2){box.x + 28 + i * 18, box.y + 152}, 5,
                    i < eng_btnmap_index()    ? GOOD
                    : i == eng_btnmap_index() ? ACCENT
                                              : LINE);

      if (GuiButton((Rectangle){box.x + 28, box.y + 172, 150, 36}, "Skip"))
        eng_btnmap_skip();
      if (GuiButton((Rectangle){box.x + 282, box.y + 172, 150, 36}, "Cancel"))
        eng_btnmap_cancel();
    }

    /* ---- new-profile modal --------------------------------------------- */
    if (name_modal) {
      Rectangle box = modal_box(440, 180, 0.06f, ACCENT);

      txt(FONTB, "New Profile", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, "Saves the current calibration under this name:", box.x + 28,
          box.y + 56, 14, DIM);
      GuiTextBox((Rectangle){box.x + 28, box.y + 80, box.width - 56, 32},
                 name_buf, 64, true);
      bool ok = name_buf[0] != 0;
      if (!ok)
        GuiDisable();
      if (GuiButton((Rectangle){box.x + 28, box.y + 126, 150, 38}, "Save")) {
        eng_profile_save_as(name_buf);
        name_modal = false;
      }
      if (!ok)
        GuiEnable();
      if (GuiButton((Rectangle){box.x + 262, box.y + 126, 150, 38}, "Cancel"))
        name_modal = false;
    }

    /* ---- reset-calibration confirm ------------------------------------- */
    if (confirm_reset) {
      Rectangle box = modal_box(440, 160, 0.06f, BAD);

      txt(FONTB, "Clear Calibration?", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT,
          TextFormat("This wipes calibration in profile \"%s\".",
                     eng_profile_current()),
          box.x + 28, box.y + 56, 14, DIM);
      if (GuiButton((Rectangle){box.x + 28, box.y + 104, 150, 38}, "Clear")) {
        eng_clear_cal();
        confirm_reset = false;
      }
      if (GuiButton((Rectangle){box.x + 262, box.y + 104, 150, 38}, "Cancel"))
        confirm_reset = false;
    }

    /* ---- statistics overlay -------------------------------------------- */
    if (show_stats) {
      Rectangle box = modal_box(700, 560, 0.03f, ACCENT);

      float bx = box.x, by = box.y;
      float colA = bx + 28, colB = bx + 252, colC = bx + 480;

      run_agg ag = compute_agg(rh_ms, rh_pr, rh_n);
      long life = eng_stats_presses();

      txt(FONTB, "Input Statistics", colA, by + 18, 22, TXT);
      time_t since = (time_t)eng_stats_since();
      char when[64] = "all time";
      if (since > 0) {
        struct tm *tmv = localtime(&since);
        if (tmv)
          strftime(when, sizeof when, "since %Y-%m-%d %H:%M", tmv);
      }
      txt(FONT, when, colA + 200, by + 26, 13, DIM);

      /* headline tiles */
      struct {
        const char *v, *l;
        Color c;
      } tile[4] = {
          {TextFormat("%ld", life), "button presses", ACCENT},
          {TextFormat("%ld", eng_stats_events()), "input events", GOOD},
          {TextFormat("%d", ag.n), "runs logged", TXT},
          {fmt_ms(ag.total_ms), "total run time", TXT},
      };
      float tx[4] = {bx + 28, bx + 196, bx + 364, bx + 512};
      for (int i = 0; i < 4; i++) {
        Rectangle tb = {tx[i] - 12, by + 46, 152, 54};
        DrawRectangleRounded(tb, 0.16f, 6, PANEL2);
        DrawRectangleRounded((Rectangle){tb.x, tb.y, 3, tb.height}, 1.0f, 4,
                             Fade(tile[i].c, 0.7f));
        txt(FONTB, tile[i].v, tx[i], by + 56, 24, tile[i].c);
        txt(FONT, tile[i].l, tx[i], by + 88, 11, DIM);
      }

      /* inset panels behind the three content columns for structure */
      DrawRectangleRounded((Rectangle){colA - 14, by + 126, 210, 250}, 0.045f,
                           6, PANEL2);
      DrawRectangleRounded((Rectangle){colB - 14, by + 126, 214, 358}, 0.045f,
                           6, PANEL2);
      DrawRectangleRounded((Rectangle){colC - 14, by + 126, 210, 358}, 0.045f,
                           6, PANEL2);

      /* --- column A: lifetime button breakdown + D-pad --- */
      section_title("Buttons (lifetime)", colA, by + 132);
      float yy = by + 158;
      if (eng_has_btnmap()) {
        for (int i = 0; i < eng_btnmap_total(); i++) {
          int code = eng_gc_code(i);
          long n = code >= 0 ? eng_stats_key(code) : 0;
          double pct = life > 0 ? 100.0 * n / life : 0.0;
          txt(FONT, eng_gc_name(i), colA, yy, 13, TXT);
          txt(FONT, TextFormat("%ld", n), colA + 96, yy, 13, DIM);
          txt(FONT, TextFormat("%4.1f%%", pct), colA + 150, yy, 12,
              Fade(DIM, 0.8f));
          yy += 19;
        }
      } else {
        int codes[64];
        int n = eng_list_keys(codes, 64), shown = 0;
        for (int i = 0; i < n && shown < 8; i++) {
          long c = eng_stats_key(codes[i]);
          if (c == 0)
            continue;
          txt(FONT, shortname(eng_code_name_key(codes[i])), colA, yy, 13, TXT);
          txt(FONT, TextFormat("%ld", c), colA + 96, yy, 13, DIM);
          yy += 19;
          shown++;
        }
        if (shown == 0)
          txt(FONT, "no presses yet", colA, yy, 13, DIM);
      }
      section_title("D-Pad", colA, by + 326);
      txt(FONT,
          TextFormat("U %ld   D %ld   L %ld   R %ld", eng_stats_dpad_count(0),
                     eng_stats_dpad_count(1), eng_stats_dpad_count(2),
                     eng_stats_dpad_count(3)),
          colA, by + 352, 13, DIM);

      /* --- column B: current/last run, fed by dusklight --- */
      section_title("Current Run", colB, by + 132);
      const char *lst = !ls_listening()   ? "server off"
                        : !ls_connected() ? "waiting for dusklight"
                        : ls_run_active() ? "RUN ACTIVE"
                                          : "connected (idle)";
      Color lsc = !ls_listening()   ? BAD
                  : !ls_connected() ? WARN
                  : ls_run_active() ? GOOD
                                    : DIM;
      txt(FONT, lst, colB, by + 158, 13, lsc);

      long live_pr = run_live ? life - run_base : last_pr;
      long live_ms = run_live ? ls_game_ms() : last_ms;
      double apm = live_ms > 0 ? live_pr * 60000.0 / live_ms : 0.0;
      double peak = run_live ? run_peak_apm : last_peak_apm;
      txt(FONTB, TextFormat("%ld", live_pr), colB, by + 176, 26,
          run_live ? GOOD : (last_pr ? TXT : DIM));
      txt(FONT, run_live ? "presses this run" : "presses (last run)", colB,
          by + 206, 12, DIM);
      txt(FONT, TextFormat("time   %s", fmt_ms(live_ms)), colB, by + 226, 13,
          DIM);
      txt(FONT, TextFormat("avg    %.0f apm", apm), colB, by + 244, 13, DIM);
      txt(FONT, TextFormat("now    %.0f apm", run_live ? cur_apm : 0.0), colB,
          by + 262, 13, DIM);
      txt(FONT, TextFormat("peak   %.0f apm", peak), colB, by + 280, 13, DIM);

      section_title("This Run by Button", colB, by + 306);
      if (!eng_has_btnmap()) {
        txt(FONT, "(map buttons first)", colB, by + 332, 12, Fade(DIM, 0.7f));
      } else {
        float byy = by + 332;
        for (int i = 0; i < eng_btnmap_total(); i++) {
          int code = eng_gc_code(i);
          long v = run_live
                       ? (code >= 0 ? eng_stats_key(code) : 0) - run_base_key[i]
                       : last_key[i];
          txt(FONT, eng_gc_name(i), colB, byy, 13, TXT);
          txt(FONT, TextFormat("%ld", v), colB + 96, byy, 13, DIM);
          byy += 18;
        }
      }

      /* --- column C: aggregate summary + recent runs --- */
      section_title("Run Summary", colC, by + 132);
      struct {
        const char *l, *v;
      } sum[] = {
          {"best", TextFormat("%ld", ag.best)},
          {"average", TextFormat("%.0f", ag.avg)},
          {"median", TextFormat("%.0f", ag.median)},
          {"worst", TextFormat("%ld", ag.worst)},
          {"best apm", TextFormat("%.0f", ag.best_apm)},
          {"avg apm", TextFormat("%.0f", ag.avg_apm)},
      };
      float syy = by + 158;
      for (int i = 0; i < 6; i++) {
        txt(FONT, sum[i].l, colC, syy, 13, DIM);
        txt(FONT, sum[i].v, colC + 110, syy, 13, i == 0 ? GOOD : TXT);
        syy += 20;
      }

      section_title("Recent Runs", colC, by + 290);
      int seli = (sel_run >= 0 && sel_run < rh_n) ? sel_run : rh_n - 1;
      if (rh_n == 0) {
        txt(FONT, "(no runs yet)", colC, by + 316, 13, Fade(DIM, 0.7f));
      } else {
        float ryy = by + 314;
        for (int i = rh_n - 1; i >= 0 && i > rh_n - 7; i--) {
          Rectangle row = {colC - 4, ryy - 2, 196, 18};
          bool hover = CheckCollisionPointRec(GetMousePosition(), row);
          if (i == seli)
            DrawRectangleRounded(row, 0.3f, 4, Fade(ACCENT, 0.18f));
          else if (hover)
            DrawRectangleRounded(row, 0.3f, 4, Fade(DIM, 0.10f));
          if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            sel_run = i;
          bool is_best = rh_pr[i] == ag.best;
          txt(FONT, TextFormat("#%d", i + 1), colC, ryy, 12, Fade(DIM, 0.8f));
          txt(FONT, fmt_ms(rh_ms[i]), colC + 34, ryy, 12, TXT);
          txt(FONT, TextFormat("%ld", rh_pr[i]), colC + 140, ryy, 12,
              is_best ? GOOD : DIM);
          ryy += 18;
        }
        /* per-run export, acting on the selected (default newest) run; the
           per-button breakdown only exists for the just-finished run */
        bool newest = seli == rh_n - 1 && !run_live && eng_has_btnmap();
        float pry = by + 440;
        if (GuiButton((Rectangle){colC, pry, 90, 30}, "Run CSV")) {
          char path[600];
          if (export_run_csv(path, sizeof path, seli + 1, rh_when[seli],
                             rh_ms[seli], rh_pr[seli], newest ? last_key : NULL,
                             newest ? eng_btnmap_total() : 0))
            snprintf(export_msg, sizeof export_msg, "saved -> %s", path);
          else
            snprintf(export_msg, sizeof export_msg, "run CSV failed");
          export_msg_t = GetTime();
        }
        if (GuiButton((Rectangle){colC + 98, pry, 90, 30}, "Run PNG")) {
          png_idx = seli + 1;
          png_when = rh_when[seli];
          png_ms = rh_ms[seli];
          png_pr = rh_pr[seli];
          png_haskey = newest;
          if (newest)
            for (int i = 0; i < eng_btnmap_total() && i < 16; i++)
              png_key[i] = last_key[i];
          png_pending = true; /* render happens after EndDrawing */
        }
      }

      /* export toast */
      if (export_msg[0] && GetTime() - export_msg_t < 6.0)
        txt(FONT, export_msg, colA, by + box.height - 82, 12, GOOD);

      float brow = by + box.height - 52;
      if (GuiButton((Rectangle){bx + 28, brow, 150, 38}, "Reset Stats"))
        confirm_stats_reset = true;
      if (GuiButton((Rectangle){bx + box.width / 2 - 75, brow, 150, 38},
                    "Export CSV")) {
        char path[600];
        if (export_stats_csv(path, sizeof path, rh_when, rh_ms, rh_pr, rh_n))
          snprintf(export_msg, sizeof export_msg, "exported -> %s", path);
        else
          snprintf(export_msg, sizeof export_msg, "export failed");
        export_msg_t = GetTime();
      }
      if (GuiButton((Rectangle){bx + box.width - 178, brow, 150, 38}, "Close"))
        show_stats = false;
    }

    /* ---- reset-statistics confirm -------------------------------------- */
    if (confirm_stats_reset) {
      Rectangle box = modal_box(440, 160, 0.06f, BAD);

      txt(FONTB, "Reset Statistics?", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, "This clears all collected input counts.", box.x + 28,
          box.y + 56, 14, DIM);
      if (GuiButton((Rectangle){box.x + 28, box.y + 104, 150, 38}, "Reset")) {
        eng_stats_reset();
        confirm_stats_reset = false;
      }
      if (GuiButton((Rectangle){box.x + 262, box.y + 104, 150, 38}, "Cancel"))
        confirm_stats_reset = false;
    }

    EndMode2D();
    EndDrawing();

    /* deferred run-card PNG export: render-to-texture must be outside the
       BeginDrawing/EndDrawing pair above */
    if (png_pending) {
      png_pending = false;
      char path[600];
      if (export_run_png(path, sizeof path, png_idx, png_when, png_ms, png_pr,
                         png_haskey ? png_key : NULL,
                         png_haskey ? eng_btnmap_total() : 0))
        snprintf(export_msg, sizeof export_msg, "saved -> %s", path);
      else
        snprintf(export_msg, sizeof export_msg, "run PNG failed");
      export_msg_t = GetTime();
    }
  }

  eng_stats_save();
  ls_stop();
  eng_io_stop();
  eng_close();
  skin_unload_all();
  CloseWindow();
  return 0;
}
