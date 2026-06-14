#pragma once
#include <stdbool.h>

#define ENG_NOTCH 8

typedef struct {
  int ax, ay;
  double cx, cy;
  double nx[ENG_NOTCH], ny[ENG_NOTCH];
} eng_stick_cal;

bool eng_open(const char *path); /* path or NULL to auto-find */
void eng_close(void);
void eng_poll(void); /* drain events; emit remapped output if active */
bool eng_connected(void);
long eng_event_count(void); /* cumulative input events seen */

int eng_axis_min(void);
int eng_axis_max(void);

bool eng_has_cal(void);
const eng_stick_cal *eng_cal(int stick); /* 0=control, 1=cstick */
void eng_measured_vec(int stick, int i, double *vx, double *vy);
void eng_ideal_vec(int stick, int i, double *wx, double *wy);
void eng_remap_point(int stick, int rx, int ry, int *ox, int *oy);

int eng_raw(int code);
bool eng_key(int code);
int eng_list_keys(int *codes, int max);
int eng_list_extra_abs(int *codes, int max);
const char *eng_code_name_key(int code);
const char *eng_code_name_abs(int code);
int eng_abs_min(int code);
int eng_abs_max(int code);

bool eng_remap_active(void);
bool eng_start_remap(void);
void eng_stop_remap(void);

double eng_get_diag(int stick);
void eng_set_diag(int stick, double d);
double eng_get_deadzone(void); /* center deadzone, fraction of stick radius */
void eng_set_deadzone(double d);
double eng_get_trig_dz(void); /* trigger bottom deadzone, fraction of range */
void eng_set_trig_dz(double d);

bool eng_load_cfg(void);
bool eng_save_cfg(void);
void eng_clear_cal(void); /* wipe calibration in the active profile */

/* profiles (~/.config/gcc-notch/profiles/<name>.conf) */
int eng_profile_count(void);
const char *eng_profile_name(int i);
const char *eng_profile_current(void);
bool eng_profile_select(const char *name);
bool eng_profile_save_as(const char *name);
void eng_profile_delete(const char *name);

/* calibration wizard */
void eng_cal_begin(void);
bool eng_cal_active(void);
int eng_cal_stick(void);
int eng_cal_phase(void); /* 0=detect axes, 1=center, 2=notches */
int eng_cal_notch(void);
void eng_cal_advance(void);
void eng_cal_cancel(void);

/* trigger calibration: rescale analog L/R so a full press hits full output */
void eng_trig_begin(void);
bool eng_trig_active(void);
int eng_trig_phase(void); /* 0=capture rest, 1=capture full press */
void eng_trig_advance(void);
void eng_trig_cancel(void);
bool eng_has_trig(void);
bool eng_is_trig(int code);          /* axis is a calibrated trigger */
int eng_trig_out(int code, int raw); /* rescaled output for preview */

/* GameCube button mapping (for the stream viewer) */
void eng_btnmap_begin(void);
bool eng_btnmap_active(void);
int eng_btnmap_index(void);        /* current step being captured */
int eng_btnmap_total(void);        /* number of GC buttons to map */
const char *eng_btnmap_name(void); /* label of the current step */
void eng_btnmap_skip(void);        /* leave current button unmapped */
void eng_btnmap_cancel(void);
bool eng_has_btnmap(void);
const char *eng_gc_name(int i); /* GC button name by index */
bool eng_gc_pressed(int i);     /* is GC button i currently pressed */
void eng_dpad(int *x, int *y);  /* D-pad direction, each -1/0/+1 */

/* device picker */
int eng_dev_count(void);
const char *eng_dev_path(void);
void eng_dev_next(void);
