#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "engine.h"
#include <math.h>
#include <raygui.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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

static Font FONT, FONTB; /* regular + semibold */

/* ---- text helpers ------------------------------------------------------ */
static void txt(Font f, const char *s, float x, float y, float sz, Color c) {
  /* snap to whole pixels so glyph edges stay crisp */
  DrawTextEx(f, s, (Vector2){roundf(x), roundf(y)}, sz, sz / 16.0f, c);
}

/* ---- shape helpers ----------------------------------------------------- */
static void card(Rectangle r) {
  DrawRectangleRounded(r, 0.045f, 8, PANEL);
  DrawRectangleRoundedLinesEx(r, 0.045f, 8, 1.0f, LINE);
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
  float w1 = MeasureTextEx(FONT, r1, 13, 13 / 16.0f).x;
  float w2 = cal ? MeasureTextEx(FONT, r2, 13, 13 / 16.0f).x : 0;
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
  return x + 16 + MeasureTextEx(FONT, label, 13, 13 / 16.0f).x + 22;
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

/* ---- GameCube stream viewer ------------------------------------------- */
static void gc_button(Vector2 c, float r, const char *label, Color col,
                      bool on) {
  if (on) {
    DrawCircleV(c, r * 1.22f, Fade(col, 0.28f));
    DrawCircleV(c, r, col);
  } else {
    DrawCircleV(c, r, Fade(col, 0.16f));
    DrawCircleLinesV(c, r, Fade(col, 0.85f));
  }
  if (label && *label) {
    float fs = r * 0.85f;
    if (fs > 24)
      fs = 24;
    if (fs < 11)
      fs = 11;
    Vector2 m = MeasureTextEx(FONTB, label, fs, fs / 16.0f);
    Color tc = on ? (Color){10, 12, 16, 255} : Fade(col, 0.95f);
    DrawTextEx(FONTB, label,
               (Vector2){roundf(c.x - m.x / 2), roundf(c.y - m.y / 2)}, fs,
               fs / 16.0f, tc);
  }
}

static void gc_capsule(Rectangle r, const char *label, Color col, bool on) {
  if (on)
    DrawRectangleRounded(r, 1.0f, 8, col);
  else {
    DrawRectangleRounded(r, 1.0f, 8, Fade(col, 0.16f));
    DrawRectangleRoundedLinesEx(r, 1.0f, 8, 1.5f, Fade(col, 0.85f));
  }
  Vector2 m = MeasureTextEx(FONTB, label, 17, 17 / 16.0f);
  DrawTextEx(FONTB, label,
             (Vector2){roundf(r.x + r.width / 2 - m.x / 2),
                       roundf(r.y + r.height / 2 - m.y / 2)},
             17, 17 / 16.0f, on ? (Color){10, 12, 16, 255} : Fade(col, 0.95f));
}

static void gc_trigger(Rectangle r, const char *label, Color col, float frac,
                       bool full) {
  DrawRectangleRounded(r, 1.0f, 8, Fade(col, 0.12f));
  if (frac > 0.001f) {
    Rectangle f = {r.x, r.y, r.width * frac, r.height};
    DrawRectangleRounded(f, 1.0f, 8, Fade(col, full ? 1.0f : 0.55f));
  }
  DrawRectangleRoundedLinesEx(r, 1.0f, 8, full ? 2.5f : 1.5f,
                              Fade(col, full ? 1.0f : 0.7f));
  txt(FONTB, label, r.x - 26, r.y + r.height / 2 - 9, 18, col);
  const char *p = TextFormat("%d%%", (int)(frac * 100 + 0.5f));
  Vector2 m = MeasureTextEx(FONT, p, 13, 13 / 16.0f);
  txt(FONT, p, r.x + r.width / 2 - m.x / 2, r.y + r.height / 2 - 7, 13,
      full ? (Color){10, 12, 16, 255} : Fade(WHITE, 0.85f));
}

static void gc_stick_view(Vector2 c, float r, int vx, int vy, Color rim,
                          Color dot, const char *label) {
  DrawCircleV(c, r, Fade(rim, 0.10f));
  DrawCircleLinesV(c, r, Fade(rim, 0.55f));
  DrawCircleLinesV(c, r * 0.5f, Fade(rim, 0.25f));
  float ox = (vx - 128) / 127.0f, oy = (vy - 128) / 127.0f;
  ox = ox < -1 ? -1 : ox > 1 ? 1 : ox;
  oy = oy < -1 ? -1 : oy > 1 ? 1 : oy;
  Vector2 p = {c.x + ox * r * 0.82f, c.y + oy * r * 0.82f};
  DrawLineEx(c, p, 2.0f, Fade(dot, 0.35f));
  DrawCircleV(p, r * 3.2f / 18.0f, Fade(dot, 0.25f));
  DrawCircleV(p, r * 2.4f / 18.0f, dot);
  if (label && *label) {
    Vector2 m = MeasureTextEx(FONT, label, 14, 14 / 16.0f);
    txt(FONT, label, c.x - m.x / 2, c.y + r + 8, 14, Fade(rim, 0.9f));
  }
}

static void gc_dpad(Vector2 c, float a) {
  int dx, dy;
  eng_dpad(&dx, &dy);
  Color base = {150, 158, 176, 255};
  float w = a * 0.74f;
  Rectangle up = {c.x - w / 2, c.y - a, w, a};
  Rectangle dn = {c.x - w / 2, c.y, w, a};
  Rectangle lf = {c.x - a, c.y - w / 2, a, w};
  Rectangle rt = {c.x, c.y - w / 2, a, w};
  DrawRectangleRounded(lf, 0.2f, 4, dx < 0 ? base : Fade(base, 0.16f));
  DrawRectangleRounded(rt, 0.2f, 4, dx > 0 ? base : Fade(base, 0.16f));
  DrawRectangleRounded(up, 0.2f, 4, dy < 0 ? base : Fade(base, 0.16f));
  DrawRectangleRounded(dn, 0.2f, 4, dy > 0 ? base : Fade(base, 0.16f));
  DrawRectangleRounded((Rectangle){c.x - w / 2, c.y - w / 2, w, w}, 0.2f, 4,
                       Fade(base, 0.16f));
}

static void draw_stream_view(int bg, bool show_hint) {
  Color bgc = bg == 1   ? (Color){0, 177, 64, 255}
              : bg == 2 ? (Color){255, 0, 255, 255}
                        : (Color){8, 8, 10, 255};
  ClearBackground(bgc);

  bool cal = eng_has_cal();
  const eng_stick_cal *c0 = eng_cal(0), *c1 = eng_cal(1);
  int rx0 = eng_raw(c0->ax), ry0 = eng_raw(c0->ay), x0 = rx0, y0 = ry0;
  int rx1 = eng_raw(c1->ax), ry1 = eng_raw(c1->ay), x1 = rx1, y1 = ry1;
  if (cal) {
    eng_remap_point(0, rx0, ry0, &x0, &y0);
    eng_remap_point(1, rx1, ry1, &x1, &y1);
  }

  /* triggers (first two calibrated analog axes -> L, R) */
  int tcodes[8], tn = 0, ac[16];
  int m = eng_list_extra_abs(ac, 16);
  for (int k = 0; k < m && tn < 8; k++)
    if (eng_is_trig(ac[k]))
      tcodes[tn++] = ac[k];
  float lf = 0, rf = 0;
  bool ld = eng_gc_pressed(5), rd = eng_gc_pressed(6);
  if (tn > 0) {
    int cd = tcodes[0], lo = eng_abs_min(cd), hi = eng_abs_max(cd);
    if (hi > lo)
      lf = (float)(eng_trig_out(cd, eng_raw(cd)) - lo) / (hi - lo);
  }
  if (tn > 1) {
    int cd = tcodes[1], lo = eng_abs_min(cd), hi = eng_abs_max(cd);
    if (hi > lo)
      rf = (float)(eng_trig_out(cd, eng_raw(cd)) - lo) / (hi - lo);
  }
  if (ld && tn == 0)
    lf = 1;
  if (rd && tn < 2)
    rf = 1;

  Color grey = {175, 182, 198, 255};
  gc_trigger((Rectangle){205, 116, 250, 34}, "L", grey, lf, ld || lf > 0.95f);
  gc_trigger((Rectangle){630, 116, 250, 34}, "R", grey, rf, rd || rf > 0.95f);
  gc_capsule((Rectangle){712, 250, 168, 28}, "Z", (Color){150, 120, 230, 255},
             eng_gc_pressed(4));

  gc_stick_view((Vector2){330, 392}, 100, x0, y0, grey, WHITE, "control");
  gc_stick_view((Vector2){700, 548}, 64, x1, y1, (Color){245, 197, 66, 255},
                (Color){245, 197, 66, 255}, "C-stick");
  gc_dpad((Vector2){300, 612}, 26);

  gc_button((Vector2){812, 380}, 46, "A", (Color){0, 180, 100, 255},
            eng_gc_pressed(0));
  gc_button((Vector2){744, 426}, 28, "B", (Color){222, 74, 74, 255},
            eng_gc_pressed(1));
  gc_button((Vector2){880, 372}, 30, "X", grey, eng_gc_pressed(2));
  gc_button((Vector2){808, 306}, 30, "Y", grey, eng_gc_pressed(3));
  gc_button((Vector2){530, 392}, 18, "", grey, eng_gc_pressed(7));
  txt(FONT, "START", 530 - MeasureTextEx(FONT, "START", 12, 12 / 16.0f).x / 2,
      392 + 24, 12, Fade(grey, 0.9f));

  if (!eng_has_btnmap())
    txt(FONT, "buttons not mapped - run \"Map Buttons\" in the main window", 24,
        24, 14, Fade(WHITE, 0.6f));
  if (show_hint)
    txt(FONT, "V exit    B background    F borderless", 24, H - 30, 14,
        Fade(WHITE, 0.5f));
}

int main(int argc, char **argv) {
  const char *devpath = NULL;
  bool daemon_mode = false, auto_remap = false, start_viewer = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--daemon") || !strcmp(argv[i], "-d"))
      daemon_mode = true;
    else if (!strcmp(argv[i], "--remap"))
      auto_remap = true;
    else if (!strcmp(argv[i], "--viewer"))
      start_viewer = true;
    else
      devpath = argv[i];
  }
  if (daemon_mode)
    return run_daemon(devpath);

  SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(W, H, "GCC Notch Remapper");
  SetTargetFPS(240);
  SetExitKey(KEY_NULL); /* we handle Esc ourselves (modal cancel / quit) */

  /* crisp TTF; fall back to the built-in font if unavailable */
  const char *reg = "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf";
  const char *bld = "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-SemiBold.ttf";
  bool gotreg = FileExists(reg), gotbld = FileExists(bld);
  /* large atlas + mipmaps + trilinear keeps small text sharp when downscaled */
  FONT = gotreg ? LoadFontEx(reg, 64, NULL, 0) : GetFontDefault();
  FONTB = gotbld ? LoadFontEx(bld, 64, NULL, 0) : FONT;
  if (gotreg) {
    GenTextureMipmaps(&FONT.texture);
    SetTextureFilter(FONT.texture, TEXTURE_FILTER_TRILINEAR);
  }
  if (gotbld) {
    GenTextureMipmaps(&FONTB.texture);
    SetTextureFilter(FONTB.texture, TEXTURE_FILTER_TRILINEAR);
  }

  /* raygui dark theme */
  GuiSetFont(FONT);
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
  if (auto_remap)
    eng_start_remap();

  /* profile dropdown + modal state (persist across frames) */
  int dd_idx = 0;
  bool dd_edit = false;
  bool name_modal = false, confirm_reset = false;
  char name_buf[64] = "";

  /* stream-view state */
  bool stream_view = start_viewer, borderless = false;
  int stream_bg = 0; /* 0 = black, 1 = chroma green, 2 = chroma magenta */
  double stream_enter = GetTime();

  while (!WindowShouldClose()) {
    eng_poll();

    /* V toggles the stream viewer in either mode */
    if (IsKeyPressed(KEY_V)) {
      stream_view = !stream_view;
      if (stream_view)
        stream_enter = GetTime();
    }

    if (stream_view) {
      if (IsKeyPressed(KEY_B))
        stream_bg = (stream_bg + 1) % 3;
      if (IsKeyPressed(KEY_F)) {
        borderless = !borderless;
        if (borderless)
          SetWindowState(FLAG_WINDOW_UNDECORATED);
        else
          ClearWindowState(FLAG_WINDOW_UNDECORATED);
      }
      if (IsKeyPressed(KEY_ESCAPE))
        stream_view = false;
    } else {
      /* keyboard shortcuts (suppressed while a modal owns the screen) */
      bool modal = eng_cal_active() || eng_trig_active() ||
                   eng_btnmap_active() || name_modal || confirm_reset;
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
        else
          break; /* quit */
      }
    }

    /* stream view replaces the whole window */
    if (stream_view) {
      BeginDrawing();
      draw_stream_view(stream_bg, GetTime() - stream_enter < 4.0);
      EndDrawing();
      continue;
    }

    BeginDrawing();
    ClearBackground(BG);

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
    const char *perf = TextFormat("%d fps   %d ev/s", GetFPS(), evrate);
    float pwf = MeasureTextEx(FONT, perf, 13, 13 / 16.0f).x;
    txt(FONT, perf, W - 28 - pwf, H - 22, 13, DIM);

    /* status pill, top-right */
    bool conn = eng_connected();
    Color sc = conn ? (eng_has_cal() ? GOOD : WARN) : BAD;
    const char *st =
        conn ? (eng_has_cal() ? "ready" : "not calibrated") : "disconnected";
    float pw = MeasureTextEx(FONT, st, 14, 14 / 16.0f).x + 44;
    Rectangle pill = {W - 28 - pw, 18, pw, 28};
    DrawRectangleRounded(pill, 1.0f, 12, PANEL2);
    DrawRectangleRoundedLinesEx(pill, 1.0f, 12, 1.0f, Fade(sc, 0.5f));
    DrawCircleV((Vector2){pill.x + 18, pill.y + 14}, 5, sc);
    txt(FONT, st, pill.x + 30, pill.y + 6, 14, TXT);

    /* REMAP ACTIVE badge (pulsing), left of the status pill */
    if (eng_remap_active()) {
      const char *bt = "REMAP ACTIVE";
      float bw = MeasureTextEx(FONT, bt, 13, 13 / 16.0f).x + 36;
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

    /* ---- right column: buttons & axes ---------------------------------- */
    Rectangle bpanel = {684, 84, W - 684 - 28, 300};
    card(bpanel);
    section_title("Buttons", bpanel.x + 16, bpanel.y + 12);
    int codes[64];
    int n = eng_list_keys(codes, 64);
    int colw = (int)(bpanel.width - 32) / 2;
    int bx = (int)bpanel.x + 16, by = (int)bpanel.y + 44, col = 0;
    for (int i = 0; i < n; i++) {
      bool on = eng_key(codes[i]);
      Rectangle led = {(float)bx, (float)by, 14, 14};
      DrawRectangleRounded(led, 0.35f, 4, on ? GOOD : PANEL2);
      if (!on)
        DrawRectangleRoundedLinesEx(led, 0.35f, 4, 1.0f, LINE);
      txt(FONT, shortname(eng_code_name_key(codes[i])), bx + 22, by - 1, 13,
          on ? TXT : DIM);
      by += 22;
      if (by > bpanel.y + bpanel.height - 24) {
        col++;
        by = (int)bpanel.y + 44;
        bx = (int)bpanel.x + 16 + col * colw;
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
    if (GuiButton((Rectangle){cp.x + 490, row3, 130, 28}, "Reset Cal."))
      confirm_reset = true;

    /* profile dropdown, drawn last so its open list overlays the row above */
    int pc = eng_profile_count();
    if (pc > 0) {
      char items[1024];
      items[0] = 0;
      int curidx = 0;
      for (int i = 0; i < pc; i++) {
        if (i)
          strncat(items, ";", sizeof items - strlen(items) - 1);
        strncat(items, eng_profile_name(i), sizeof items - strlen(items) - 1);
        if (!strcmp(eng_profile_name(i), eng_profile_current()))
          curidx = i;
      }
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

    /* row4: stream viewer */
    float row4 = cp.y + 168;
    if (GuiButton((Rectangle){cp.x + 16, row4, 290, 30},
                  "Open Stream View  (V)")) {
      stream_view = true;
      stream_enter = GetTime();
    }
    if (!eng_has_btnmap())
      txt(FONT, "tip: Map Buttons first for the GameCube layout", cp.x + 320,
          row4 + 8, 12, DIM);

    /* ---- calibration overlay ------------------------------------------- */
    if (eng_cal_active()) {
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 250, H / 2.0f - 120, 500, 240};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(ACCENT, 0.6f));

      const char *sn = eng_cal_stick() == 0 ? "Control Stick" : "C-Stick";
      int ph = eng_cal_phase();
      txt(FONTB, "Calibration", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, sn, box.x + 28, box.y + 52, 14, ACCENT);

      const char *msg;
      char buf[160];
      if (ph == 0)
        msg = "Spin the stick fully around its gate, then press Next.";
      else if (ph == 1)
        msg = "Let the stick rest at center, then press Capture.";
      else {
        snprintf(buf, sizeof buf, "Hold notch %d / 8, then press Capture.",
                 eng_cal_notch() + 1);
        msg = buf;
      }
      txt(FONT, msg, box.x + 28, box.y + 92, 16, TXT);

      /* step dots */
      for (int i = 0; i < 3; i++)
        DrawCircleV((Vector2){box.x + 28 + i * 16, box.y + 128}, 4,
                    i <= (ph > 2 ? 2 : ph) ? ACCENT : LINE);

      const eng_stick_cal *c = eng_cal(eng_cal_stick());
      txt(FONT, TextFormat("live  (%d, %d)", eng_raw(c->ax), eng_raw(c->ay)),
          box.x + 28, box.y + 150, 14, DIM);

      if (GuiButton((Rectangle){box.x + 28, box.y + 182, 150, 38},
                    ph == 0 ? "Next" : "Capture"))
        eng_cal_advance();
      if (GuiButton((Rectangle){box.x + 322, box.y + 182, 150, 38}, "Cancel"))
        eng_cal_cancel();
    }

    /* ---- trigger calibration overlay ----------------------------------- */
    if (eng_trig_active()) {
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 250, H / 2.0f - 130, 500, 260};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(ACCENT, 0.6f));

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
      float ly = box.y + 138;
      for (int k = 0; k < m2; k++) {
        int code = ac2[k], lo = eng_abs_min(code), hi = eng_abs_max(code);
        if (hi - lo < 16)
          continue;
        float f = (float)(eng_raw(code) - lo) / (hi - lo);
        txt(FONT, shortname(eng_code_name_abs(code)), box.x + 28, ly, 13, DIM);
        Rectangle bar = {box.x + 150, ly + 1, 220, 10};
        DrawRectangleRounded(bar, 1.0f, 6, PANEL2);
        if (f > 0.001f)
          DrawRectangleRounded(
              (Rectangle){bar.x, bar.y, bar.width * f, bar.height}, 1.0f, 6,
              ACCENT);
        ly += 20;
        if (ly > box.y + 188)
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
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 230, H / 2.0f - 110, 460, 220};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(ACCENT, 0.6f));
      txt(FONTB, "Map Buttons", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT,
          TextFormat("Step %d / %d", eng_btnmap_index() + 1,
                     eng_btnmap_total()),
          box.x + 28, box.y + 54, 14, DIM);

      const char *gcn = eng_btnmap_name();
      txt(FONT, "Press your", box.x + 28, box.y + 92, 16, TXT);
      txt(FONTB, gcn, box.x + 150, box.y + 88, 26, ACCENT);
      txt(FONT, "button",
          box.x + 150 + MeasureTextEx(FONTB, gcn, 26, 26 / 16.0f).x + 12,
          box.y + 92, 16, TXT);
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
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 220, H / 2.0f - 90, 440, 180};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(ACCENT, 0.6f));
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
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 220, H / 2.0f - 80, 440, 160};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(BAD, 0.6f));
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

    EndDrawing();
  }

  eng_close();
  CloseWindow();
  return 0;
}
