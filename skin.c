#include "engine.h"
#include "raylib.h"
#include <dirent.h>
#include <libxml2/libxml/parser.h>
#include <libxml2/libxml/tree.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/stat.h>
#include <unistd.h>

#define SKIN_MAX 32
#define EL_MAX 64

typedef enum { EL_BUTTON, EL_STICK, EL_ANALOG } el_kind;

typedef struct {
  el_kind kind;
  Texture2D tex;
  float x, y, w, h;     /* layout rect, background-pixel space */
  int gc;               /* gc button index 0..7, else -1 */
  int dpad;             /* 1=up 2=down 3=left 4=right, else 0 */
  int stick;            /* 0=control 1=cstick, else -1 */
  float xrange, yrange; /* stick max pixel travel */
  int trig;             /* 1=L 2=R, else 0 */
  int dir;              /* analog reveal: 0=right 1=left 2=down 3=up */
  bool reverse;
} skin_el;

typedef struct {
  char dir[512];
  char name[64];
  char author[64];
  Texture2D bg;
  float bgw, bgh;
  skin_el el[EL_MAX];
  int nel;
} skin_t;

static skin_t skins[SKIN_MAX];
static int nskins = 0, cur = -1;

/* ---- paths ---- */
static void skins_dir(char *b, size_t n) {
  const char *h = getenv("HOME");
  snprintf(b, n, "%s/.config/gcc-notch/skins", h ? h : ".");
}
static void choice_path(char *b, size_t n) {
  const char *h = getenv("HOME");
  snprintf(b, n, "%s/.config/gcc-notch/skin", h ? h : ".");
}

/* ---- xml attribute helpers ---- */
static void prop_str(xmlNode *n, const char *k, char *out, size_t cap) {
  out[0] = 0;
  xmlChar *v = xmlGetProp(n, (const xmlChar *)k);
  if (v) {
    snprintf(out, cap, "%s", (const char *)v);
    xmlFree(v);
  }
}

static float prop_f(xmlNode *n, const char *k) {
  char b[64];
  prop_str(n, k, b, sizeof b);
  return (float)atof(b);
}

static Texture2D load_img(const char *dir, xmlNode *n) {
  char img[256], path[800];
  prop_str(n, "image", img, sizeof img);
  snprintf(path, sizeof path, "%s/%s", dir, img);
  Texture2D t = LoadTexture(path);
  if (t.id) {
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_CLAMP);
  }
  return t;
}

/* ---- name -> engine input ---- */
static void map_button(const char *nm, skin_el *e) {
  static const char *gcn[8] = {"a", "b", "x", "y", "z", "l", "r", "start"};
  for (int i = 0; i < 8; i++)
    if (!strcasecmp(nm, gcn[i])) {
      e->gc = i;
      return;
    }
  if (!strcasecmp(nm, "up"))
    e->dpad = 1;
  else if (!strcasecmp(nm, "down"))
    e->dpad = 2;
  else if (!strcasecmp(nm, "left"))
    e->dpad = 3;
  else if (!strcasecmp(nm, "right"))
    e->dpad = 4;
}

/* same L/R fraction logic as the old stream viewer */
static float trig_frac(int which) { /* which: 1=L 2=R */
  int ac[16], n = eng_list_extra_abs(ac, 16), idx = 0;
  for (int k = 0; k < n; k++) {
    if (!eng_is_trig(ac[k]))
      continue;
    if (++idx == which) {
      int cd = ac[k], lo = eng_abs_min(cd), hi = eng_abs_max(cd);
      if (hi > lo) {
        float f = (float)(eng_trig_out(cd, eng_raw(cd)) - lo) / (hi - lo);
        return f < 0 ? 0 : f > 1 ? 1 : f;
      }
    }
  }
  return eng_gc_pressed(which == 1 ? 5 : 6) ? 1.0f
                                            : 0.0f; /* digital fallback */
}

static void parse_skin(const char *dir, skin_t *s) {
  char path[800];
  snprintf(path, sizeof path, "%s/skin.xml", dir);
  xmlDoc *doc = xmlReadFile(path, NULL, 0);
  if (!doc)
    return;
  xmlNode *root = xmlDocGetRootElement(doc);
  if (!root) {
    xmlFreeDoc(doc);
    return;
  }

  snprintf(s->dir, sizeof s->dir, "%s", dir);
  prop_str(root, "name", s->name, sizeof s->name);
  if (!s->name[0])
    snprintf(s->name, sizeof s->name, "%s", GetFileName(dir));
  prop_str(root, "author", s->author, sizeof s->author);

  for (xmlNode *n = root->children; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE)
      continue;
    const char *tag = (const char *)n->name;

    if (!strcmp(tag, "background")) {
      s->bg = load_img(dir, n);
      s->bgw = (float)s->bg.width;
      s->bgh = (float)s->bg.height;
      continue;
    }
    if (s->nel >= EL_MAX)
      continue;
    skin_el *e = &s->el[s->nel];
    memset(e, 0, sizeof *e);
    e->gc = -1;
    e->stick = -1;
    e->x = prop_f(n, "x");
    e->y = prop_f(n, "y");
    e->w = prop_f(n, "width");
    e->h = prop_f(n, "height");

    char nm[64];
    if (!strcmp(tag, "button")) {
      e->kind = EL_BUTTON;
      prop_str(n, "name", nm, sizeof nm);
      map_button(nm, e);
      e->tex = load_img(dir, n);
      s->nel++;
    } else if (!strcmp(tag, "stick")) {
      e->kind = EL_STICK;
      prop_str(n, "xname", nm, sizeof nm);
      e->stick = !strncasecmp(nm, "c", 1) ? 1 : 0; /* cstick_x vs lstick_x */
      e->xrange = prop_f(n, "xrange");
      e->yrange = prop_f(n, "yrange");
      e->tex = load_img(dir, n);
      s->nel++;
    } else if (!strcmp(tag, "analog")) {
      e->kind = EL_ANALOG;
      prop_str(n, "name", nm, sizeof nm);
      e->trig = !strcasecmp(nm, "trig_l")   ? 1
                : !strcasecmp(nm, "trig_r") ? 2
                                            : 0;
      char d[16];
      prop_str(n, "direction", d, sizeof d);
      e->dir = !strcasecmp(d, "left")   ? 1
               : !strcasecmp(d, "down") ? 2
               : !strcasecmp(d, "up")   ? 3
                                        : 0; /* default = right */
      char rv[16];
      prop_str(n, "reverse", rv, sizeof rv);
      e->reverse = !strcasecmp(rv, "true");
      e->tex = load_img(dir, n);
      s->nel++;
    }
  }
  xmlFreeDoc(doc);
}

/* ---- selection persistence ---- */
static void save_choice(void) {
  if (cur < 0)
    return;
  char p[600];
  choice_path(p, sizeof p);
  FILE *f = fopen(p, "w");
  if (f) {
    fprintf(f, "%s\n", GetFileName(skins[cur].dir));
    fclose(f);
  }
}
static void load_choice(char *out, size_t cap) {
  out[0] = 0;
  char p[600];
  choice_path(p, sizeof p);
  FILE *f = fopen(p, "r");
  if (f) {
    if (fgets(out, cap, f))
      out[strcspn(out, "\r\n")] = 0;
    fclose(f);
  }
}

int skin_load_all(void) {
  char dir[512];
  skins_dir(dir, sizeof dir);
  { /* make sure the folder exists so users have somewhere to drop skins */
    const char *h = getenv("HOME");
    char p[600];
    snprintf(p, sizeof p, "%s/.config/gcc-notch", h ? h : ".");
    mkdir(p, 0755);
    mkdir(dir, 0755);
  }

  DIR *d = opendir(dir);
  if (!d)
    return 0;
  struct dirent *de;
  while ((de = readdir(d)) && nskins < SKIN_MAX) {
    if (de->d_name[0] == '.')
      continue;
    char sub[800], xml[900];
    snprintf(sub, sizeof sub, "%s/%s", dir, de->d_name);
    struct stat st;
    if (stat(sub, &st) || !S_ISDIR(st.st_mode))
      continue;
    snprintf(xml, sizeof xml, "%s/skin.xml", sub);
    if (access(xml, R_OK))
      continue;
    skin_t *s = &skins[nskins];
    memset(s, 0, sizeof *s);
    parse_skin(sub, s);
    if (s->bg.id) /* keep only skins with a valid background */
      nskins++;
  }
  closedir(d);

  if (nskins > 0) {
    cur = 0;
    char want[64];
    load_choice(want, sizeof want);
    for (int i = 0; i < nskins; i++)
      if (!strcmp(GetFileName(skins[i].dir), want)) {
        cur = i;
        break;
      }
  }
  return nskins;
}

void skin_unload_all(void) {
  for (int i = 0; i < nskins; i++) {
    if (skins[i].bg.id)
      UnloadTexture(skins[i].bg);
    for (int k = 0; k < skins[i].nel; k++)
      if (skins[i].el[k].tex.id)
        UnloadTexture(skins[i].el[k].tex);
  }
  nskins = 0;
  cur = -1;
  xmlCleanupParser();
}

int skin_count(void) { return nskins; }

const char *skin_name(int i) {
  return (i >= 0 && i < nskins) ? skins[i].name : "";
}

const char *skin_author(int i) {
  return (i >= 0 && i < nskins) ? skins[i].author : "";
}

void skin_reload(void) {
  skin_unload_all();
  skin_load_all();
}

int skin_current(void) { return cur; }
bool skin_have(void) { return cur >= 0 && cur < nskins; }
bool skin_size(int *w, int *h) {
  if (cur < 0 || cur >= nskins)
    return false;
  *w = (int)skins[cur].bgw;
  *h = (int)skins[cur].bgh;
  return true;
}
void skin_select(int i) {
  if (i >= 0 && i < nskins) {
    cur = i;
    save_choice();
  }
}
void skin_next(void) {
  if (nskins > 0)
    skin_select((cur + 1) % nskins);
}

static Font skin_font;
static bool skin_have_font;
static bool skin_show_values = true;   /* toggled from the viewer (N key) */
static bool skin_remap_display = true; /* false: show the source device raw */
void skin_set_font(Font f) {
  skin_font = f;
  skin_have_font = true;
}
void skin_set_values(bool on) { skin_show_values = on; }
void skin_set_remap_display(bool on) { skin_remap_display = on; }

void skin_draw(int win_w, int win_h) {
  if (cur < 0 || cur >= nskins)
    return;
  skin_t *s = &skins[cur];
  if (s->bgw < 1 || s->bgh < 1)
    return;

  float scale = fminf(win_w / s->bgw, win_h / s->bgh);
  float ox = (win_w - s->bgw * scale) * 0.5f;
  float oy = (win_h - s->bgh * scale) * 0.5f;
#define DST(X, Y, Wd, Hd)                                                      \
  (Rectangle){ox + (X) * scale, oy + (Y) * scale, (Wd) * scale, (Hd) * scale}

  DrawTexturePro(s->bg, (Rectangle){0, 0, s->bgw, s->bgh},
                 DST(0, 0, s->bgw, s->bgh), (Vector2){0, 0}, 0, WHITE);

  bool cal = eng_has_cal();
  for (int i = 0; i < s->nel; i++) {
    skin_el *e = &s->el[i];
    Rectangle full = {0, 0, (float)e->tex.width, (float)e->tex.height};

    if (e->kind == EL_BUTTON) {
      bool on = false;
      if (e->gc >= 0)
        on = eng_gc_pressed(e->gc);
      else if (e->dpad) {
        int dx, dy;
        eng_dpad(&dx, &dy);
        on = (e->dpad == 1 && dy < 0) || (e->dpad == 2 && dy > 0) ||
             (e->dpad == 3 && dx < 0) || (e->dpad == 4 && dx > 0);
      }
      if (on)
        DrawTexturePro(e->tex, full, DST(e->x, e->y, e->w, e->h),
                       (Vector2){0, 0}, 0, WHITE);

    } else if (e->kind == EL_STICK) {
      const eng_stick_cal *c = eng_cal(e->stick);
      int rx = eng_raw(c->ax), ry = eng_raw(c->ay), vx = rx, vy = ry;
      if (cal && skin_remap_display)
        eng_remap_point(e->stick, rx, ry, &vx, &vy);
      float nx = (vx - 128) / 127.0f, ny = (vy - 128) / 127.0f;
      nx = nx < -1 ? -1 : nx > 1 ? 1 : nx;
      ny = ny < -1 ? -1 : ny > 1 ? 1 : ny;
      Rectangle sr =
          DST(e->x + nx * e->xrange, e->y + ny * e->yrange, e->w, e->h);
      DrawTexturePro(e->tex, full, sr, (Vector2){0, 0}, 0, WHITE);
      if (skin_show_values) { /* live value, centered on the (moving) image */
        Font f = skin_have_font ? skin_font : GetFontDefault();
        const char *str = TextFormat("%d,%d", vx, vy);
        float fs = sr.height * 0.22f;
        fs = fs < 10 ? 10 : fs > 24 ? 24 : fs;
        Vector2 m = MeasureTextEx(f, str, fs, 1);
        Vector2 p = {sr.x + (sr.width - m.x) / 2, sr.y + (sr.height - m.y) / 2};
        DrawTextEx(f, str, (Vector2){p.x + 1, p.y + 1}, fs, 1,
                   Fade(BLACK, 0.75f));
        DrawTextEx(f, str, p, fs, 1, WHITE);
      }

    } else { /* EL_ANALOG */
      float f = trig_frac(e->trig);
      if (e->reverse)
        f = 1.0f - f;
      if (f <= 0.001f)
        continue;
      Rectangle src, dst;
      switch (e->dir) {
      case 1: /* left: anchored right, reveal leftward */
        src = (Rectangle){full.width * (1 - f), 0, full.width * f, full.height};
        dst = DST(e->x + e->w * (1 - f), e->y, e->w * f, e->h);
        break;
      case 2: /* down: anchored top, reveal downward */
        src = (Rectangle){0, 0, full.width, full.height * f};
        dst = DST(e->x, e->y, e->w, e->h * f);
        break;
      case 3: /* up: anchored bottom, reveal upward */
        src =
            (Rectangle){0, full.height * (1 - f), full.width, full.height * f};
        dst = DST(e->x, e->y + e->h * (1 - f), e->w, e->h * f);
        break;
      default: /* right: anchored left, reveal rightward */
        src = (Rectangle){0, 0, full.width * f, full.height};
        dst = DST(e->x, e->y, e->w * f, e->h);
        break;
      }
      DrawTexturePro(e->tex, src, dst, (Vector2){0, 0}, 0, WHITE);
    }
  }
#undef DST
}
