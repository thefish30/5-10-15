#include <pebble.h>

// ============================================================
// 5/10/15  v1.3
//
// New in v1.3:
//   - Phone-side Clay configuration for lists and tasks
//   - Palette updated to match Pebblefocus/Pooble conventions
//   - AppMessage inbox receives new library from phone, preserves
//     active list by name if it still exists
//   - REAL-TIME TRACKING + WAKEUP CHAINING:
//     Every running task is anchored to a wall-clock start time.
//     Remaining time is always (duration - (now - anchor)), so
//     reopening the app - by tap or by a scheduled wakeup - shows
//     the honest current state, never a frozen/paused guess.
//     Every task, looping or not, schedules a wakeup for exactly
//     when it should end. If the app isn't open when that fires,
//     the OS relaunches it, it advances for real, vibrates, and
//     schedules the next one. Pausing is the one deliberate way
//     to freeze time; it cancels the pending wakeup.
//
// Keeps v1.2's per-list persistence fix.
//
// v1.4: "then run" list chaining. A non-looping list can name a
//   list to start when it finishes (stored as then_run). Works
//   foreground and through wakeup/resync. No schema bump: the
//   then_run byte is appended trailing, absent in old saves.
// ============================================================

// ============================================================
// PERSIST KEYS
// ============================================================
#define PERSIST_KEY_OLD_LIBRARY    10   // legacy blob (deleted on migration)
#define PERSIST_KEY_ACTIVE_LIST    11
#define PERSIST_KEY_CURRENT_TASK   12
#define PERSIST_KEY_REMAINING      13   // authoritative only while PAUSED
#define PERSIST_KEY_STATE          14
#define PERSIST_KEY_LAST_ACTIVITY  15
#define PERSIST_KEY_VERSION        16
#define PERSIST_KEY_LIST_COUNT     17
#define PERSIST_KEY_TASK_ANCHOR    18   // wall-clock time current task started
#define PERSIST_KEY_WAKEUP_ID      19
#define PERSIST_KEY_LIST_BASE      20   // each list at key (LIST_BASE + index)

#define SCHEMA_VERSION  8

// ============================================================
// LIMITS
// ============================================================
#define MAX_LISTS        10
#define MAX_TASKS        10
#define MAX_LIST_NAME    16
#define MAX_TASK_NAME    20
#define PALETTE_SIZE     7

#define LIST_BLOB_MAX    256   // per-key Pebble limit

#define RESUME_WINDOW_SEC (60 * 60)
#define DONE_SCREEN_SEC   8
#define WAKEUP_MIN_LEAD_SEC 30  // Pebble refuses wakeups scheduled sooner than this
#define MAX_RESYNC_HOPS   500   // safety cap on the catch-up loop

#define MINUTES(m) ((m) * 60)

#define INBOX_SIZE   2048
#define OUTBOX_SIZE  128

// ============================================================
// PALETTE  (Pebblefocus/Pooble conventions)
// ============================================================
#define C_PURPLE 0
#define C_BLUE   1
#define C_GREEN  2
#define C_YELLOW 3
#define C_AQUA   4
#define C_PINK   5
#define C_RED    6

static GColor task_color(uint8_t color_idx) {
  switch (color_idx) {
    case C_PURPLE: return GColorVividViolet;
    case C_BLUE:   return GColorBlue;
    case C_GREEN:  return GColorIslamicGreen;
    case C_YELLOW: return GColorYellow;
    case C_AQUA:   return GColorCyan;
    case C_PINK:   return GColorShockingPink;
    case C_RED:    return GColorRed;
    default:       return GColorDarkGray;
  }
}

// ============================================================
// DATA MODEL
// ============================================================
typedef struct {
  char     name[MAX_TASK_NAME];
  uint32_t duration;   // seconds
  uint8_t  color;
} Task;

typedef struct {
  char    name[MAX_LIST_NAME];
  uint8_t task_count;
  uint8_t protected_flag;
  uint8_t loops;
  uint8_t then_run;   // 0 = none, else target list index + 1
  Task    tasks[MAX_TASKS];
} TaskList;

typedef struct {
  uint8_t  list_count;
  TaskList lists[MAX_LISTS];
} Library;

static Library  s_library;
static uint8_t  s_active_list = 0;
static uint8_t  s_current_task = 0;
static uint32_t s_remaining = 0;      // authoritative while PAUSED; derived while RUNNING
static time_t   s_task_anchor = 0;    // wall-clock time current task started running
static time_t   s_last_activity = 0;
static WakeupId s_wakeup_id = -1;

typedef enum {
  STATE_IDLE,
  STATE_RUNNING,
  STATE_PAUSED,
  STATE_DONE,
  STATE_EMPTY       // no lists exist
} AppState;
static AppState s_state = STATE_IDLE;

static AppTimer *s_done_timer = NULL;
static uint8_t   s_done_countdown = 0;

// ============================================================
// UI HANDLES
// ============================================================
static Window     *s_timer_window;
static TextLayer  *s_name_layer;
static TextLayer  *s_time_layer;
static TextLayer  *s_status_layer;
static TextLayer  *s_position_layer;
static TextLayer  *s_listname_layer;
static Layer      *s_progress_layer;

static Window     *s_picker_window;
static MenuLayer  *s_picker_menu;

static char s_time_buf[16];
static char s_pos_buf[16];

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
static void save_state(void);
static void save_library(void);
static void save_all(void);
static void touch_activity(void);
static void build_default_library(void);
static void timer_render_all(void);
static void show_picker(void);
static void hide_picker(void);
static void activate_list(uint8_t idx);
static void load_current_task_display(void);
static void enter_done_state(void);
static void exit_done_state(void);
static void cancel_wakeup(void);
static void schedule_wakeup_for_remaining(void);
static void resync_running_state(void);

// ============================================================
// DEFAULT LIBRARY  (colors mapped to new 7-color palette)
// ============================================================
static void set_list(uint8_t li, const char *name, uint8_t protected_flag, uint8_t loops) {
  TaskList *l = &s_library.lists[li];
  strncpy(l->name, name, MAX_LIST_NAME - 1);
  l->name[MAX_LIST_NAME - 1] = '\0';
  l->task_count = 0;
  l->protected_flag = protected_flag;
  l->loops = loops;
  l->then_run = 0;
}

static void add_task(uint8_t li, const char *name, uint32_t duration_sec, uint8_t color) {
  TaskList *l = &s_library.lists[li];
  if (l->task_count >= MAX_TASKS) return;
  Task *t = &l->tasks[l->task_count++];
  strncpy(t->name, name, MAX_TASK_NAME - 1);
  t->name[MAX_TASK_NAME - 1] = '\0';
  t->duration = duration_sec;
  t->color = color;
}

static void build_default_library(void) {
  s_library.list_count = 0;

  set_list(0, "Work Loop", 0, 1);
  add_task(0, "House",   MINUTES(15), C_PURPLE);
  add_task(0, "Desk",    MINUTES(15), C_BLUE);
  add_task(0, "Special", MINUTES(15), C_YELLOW);
  add_task(0, "Rest",    MINUTES(15), C_GREEN);

  set_list(1, "Quick Loop", 0, 1);
  add_task(1, "House",   MINUTES(5), C_PURPLE);
  add_task(1, "Desk",    MINUTES(5), C_BLUE);
  add_task(1, "Special", MINUTES(5), C_YELLOW);
  add_task(1, "Rest",    MINUTES(5), C_GREEN);

  set_list(2, "Long Loop", 0, 1);
  add_task(2, "House",   MINUTES(30), C_PURPLE);
  add_task(2, "Desk",    MINUTES(30), C_BLUE);
  add_task(2, "Special", MINUTES(30), C_YELLOW);
  add_task(2, "Rest",    MINUTES(30), C_GREEN);

  set_list(3, "Morning", 0, 0);
  add_task(3, "Get up",    MINUTES(5),  C_AQUA);
  add_task(3, "Teeth",     MINUTES(5),  C_AQUA);
  add_task(3, "Shower",    MINUTES(10), C_BLUE);
  add_task(3, "Dress",     MINUTES(10), C_PURPLE);
  add_task(3, "Hair",      MINUTES(10), C_PINK);
  add_task(3, "Breakfast", MINUTES(15), C_YELLOW);
  add_task(3, "Go!",       MINUTES(5),  C_GREEN);

  s_library.list_count = 4;
}

// ============================================================
// ACCESSORS
// ============================================================
static TaskList* active(void) {
  return &s_library.lists[s_active_list];
}

static Task* current_task(void) {
  return &active()->tasks[s_current_task];
}

// ============================================================
// PER-LIST SERIALIZATION (v1.2 fix, unchanged)
// ============================================================
static void save_one_list(uint8_t idx, const TaskList *l) {
  uint8_t buf[LIST_BLOB_MAX];
  int p = 0;

  uint8_t nlen = strlen(l->name);
  if (nlen >= MAX_LIST_NAME) nlen = MAX_LIST_NAME - 1;
  buf[p++] = nlen;
  for (int i = 0; i < nlen; i++) buf[p++] = (uint8_t)l->name[i];

  buf[p++] = l->loops ? 1 : 0;
  buf[p++] = l->task_count;

  for (uint8_t i = 0; i < l->task_count && i < MAX_TASKS; i++) {
    const Task *t = &l->tasks[i];
    uint8_t tnlen = strlen(t->name);
    if (tnlen >= MAX_TASK_NAME) tnlen = MAX_TASK_NAME - 1;
    buf[p++] = tnlen;
    for (int j = 0; j < tnlen; j++) buf[p++] = (uint8_t)t->name[j];
    uint32_t minutes = t->duration / 60;
    if (minutes < 1) minutes = 1;
    if (minutes > 255) minutes = 255;
    buf[p++] = (uint8_t)minutes;
    buf[p++] = t->color;
  }

  buf[p++] = l->then_run;   // trailing: chain target (0 = none)

  persist_write_data(PERSIST_KEY_LIST_BASE + idx, buf, p);
}

static bool load_one_list(uint8_t idx, TaskList *l) {
  uint32_t key = PERSIST_KEY_LIST_BASE + idx;
  if (!persist_exists(key)) return false;

  uint8_t buf[LIST_BLOB_MAX];
  int n = persist_read_data(key, buf, sizeof(buf));
  if (n <= 0) return false;

  int p = 0;
  if (p >= n) return false;

  uint8_t nlen = buf[p++];
  if (nlen >= MAX_LIST_NAME) nlen = MAX_LIST_NAME - 1;
  if (p + nlen > n) return false;
  for (int i = 0; i < nlen; i++) l->name[i] = (char)buf[p++];
  l->name[nlen] = '\0';

  if (p + 2 > n) return false;
  l->loops = buf[p++];
  l->task_count = buf[p++];
  if (l->task_count > MAX_TASKS) l->task_count = MAX_TASKS;
  l->protected_flag = 0;
  l->then_run = 0;

  for (uint8_t i = 0; i < l->task_count; i++) {
    Task *t = &l->tasks[i];
    if (p + 1 > n) return false;
    uint8_t tnlen = buf[p++];
    if (tnlen >= MAX_TASK_NAME) tnlen = MAX_TASK_NAME - 1;
    if (p + tnlen > n) return false;
    for (int j = 0; j < tnlen; j++) t->name[j] = (char)buf[p++];
    t->name[tnlen] = '\0';

    if (p + 2 > n) return false;
    uint8_t minutes = buf[p++];
    uint8_t color = buf[p++];
    if (minutes < 1) minutes = 1;
    if (color >= PALETTE_SIZE) color = 0;
    t->duration = (uint32_t)minutes * 60;
    t->color = color;
  }

  // then_run: optional trailing byte (absent in pre-chaining saves)
  if (p < n) {
    l->then_run = buf[p++];
    if (l->then_run > MAX_LISTS) l->then_run = 0;
  }

  return true;
}

// ============================================================
// PERSISTENCE
// ============================================================
static void save_state(void) {
  persist_write_int(PERSIST_KEY_VERSION, SCHEMA_VERSION);
  persist_write_int(PERSIST_KEY_ACTIVE_LIST, s_active_list);
  persist_write_int(PERSIST_KEY_CURRENT_TASK, s_current_task);
  persist_write_int(PERSIST_KEY_REMAINING, s_remaining);
  persist_write_int(PERSIST_KEY_TASK_ANCHOR, (int)s_task_anchor);
  persist_write_int(PERSIST_KEY_WAKEUP_ID, (int)s_wakeup_id);
  AppState persist_state = (s_state == STATE_DONE) ? STATE_PAUSED : s_state;
  persist_write_int(PERSIST_KEY_STATE, persist_state);
  persist_write_int(PERSIST_KEY_LAST_ACTIVITY, (int)s_last_activity);
}

static void save_library(void) {
  persist_write_int(PERSIST_KEY_VERSION, SCHEMA_VERSION);
  persist_write_int(PERSIST_KEY_LIST_COUNT, s_library.list_count);
  for (uint8_t i = 0; i < s_library.list_count; i++) {
    save_one_list(i, &s_library.lists[i]);
  }
  for (uint8_t i = s_library.list_count; i < MAX_LISTS; i++) {
    persist_delete(PERSIST_KEY_LIST_BASE + i);
  }
}

static void save_all(void) {
  save_library();
  save_state();
}

static void touch_activity(void) {
  s_last_activity = time(NULL);
  persist_write_int(PERSIST_KEY_LAST_ACTIVITY, (int)s_last_activity);
}

static void load_all(void) {
  int saved_version = persist_exists(PERSIST_KEY_VERSION)
    ? persist_read_int(PERSIST_KEY_VERSION) : 0;

  if (saved_version != SCHEMA_VERSION) {
    if (persist_exists(PERSIST_KEY_OLD_LIBRARY)) {
      persist_delete(PERSIST_KEY_OLD_LIBRARY);
    }
    for (uint8_t i = 0; i < MAX_LISTS; i++) {
      persist_delete(PERSIST_KEY_LIST_BASE + i);
    }
    build_default_library();
    s_active_list = 0;
    s_current_task = 0;
    s_remaining = active()->tasks[0].duration;
    s_task_anchor = time(NULL);
    s_wakeup_id = -1;
    s_state = STATE_IDLE;
    s_last_activity = 0;
    save_all();
    return;
  }

  s_library.list_count = 0;
  if (persist_exists(PERSIST_KEY_LIST_COUNT)) {
    uint8_t declared = (uint8_t)persist_read_int(PERSIST_KEY_LIST_COUNT);
    if (declared > MAX_LISTS) declared = MAX_LISTS;
    for (uint8_t i = 0; i < declared; i++) {
      if (load_one_list(i, &s_library.lists[i])) {
        s_library.list_count++;
      } else {
        break;
      }
    }
  }

  if (s_library.list_count == 0) {
    s_state = STATE_EMPTY;
    s_active_list = 0;
    s_current_task = 0;
    s_remaining = 0;
    s_task_anchor = time(NULL);
    s_wakeup_id = -1;
    return;
  }

  s_active_list = persist_exists(PERSIST_KEY_ACTIVE_LIST)
    ? persist_read_int(PERSIST_KEY_ACTIVE_LIST) : 0;
  if (s_active_list >= s_library.list_count) s_active_list = 0;

  s_current_task = persist_exists(PERSIST_KEY_CURRENT_TASK)
    ? persist_read_int(PERSIST_KEY_CURRENT_TASK) : 0;
  if (s_current_task >= active()->task_count) s_current_task = 0;

  s_remaining = persist_exists(PERSIST_KEY_REMAINING)
    ? (uint32_t)persist_read_int(PERSIST_KEY_REMAINING) : current_task()->duration;
  if (s_remaining > current_task()->duration) {
    s_remaining = current_task()->duration;
  }

  s_task_anchor = persist_exists(PERSIST_KEY_TASK_ANCHOR)
    ? (time_t)persist_read_int(PERSIST_KEY_TASK_ANCHOR) : time(NULL);

  s_wakeup_id = persist_exists(PERSIST_KEY_WAKEUP_ID)
    ? (WakeupId)persist_read_int(PERSIST_KEY_WAKEUP_ID) : -1;

  if (persist_exists(PERSIST_KEY_STATE)) {
    s_state = persist_read_int(PERSIST_KEY_STATE);
  } else {
    s_state = STATE_IDLE;
  }

  s_last_activity = persist_exists(PERSIST_KEY_LAST_ACTIVITY)
    ? (time_t)persist_read_int(PERSIST_KEY_LAST_ACTIVITY) : 0;
}

// ============================================================
// VIBRATION
// ============================================================
static void vibe_advance(void) {
  static const uint32_t p[] = {100, 100, 100};
  VibePattern vp = { .durations = p, .num_segments = 3 };
  vibes_enqueue_custom_pattern(vp);
}

static void vibe_complete(void) {
  static const uint32_t p[] = {500, 200, 200, 200, 500};
  VibePattern vp = { .durations = p, .num_segments = 5 };
  vibes_enqueue_custom_pattern(vp);
}

// ============================================================
// WAKEUP SCHEDULING
// ============================================================
static void cancel_wakeup(void) {
  if (s_wakeup_id >= 0) {
    wakeup_cancel(s_wakeup_id);
  }
  s_wakeup_id = -1;
}

// Schedules a wakeup for exactly when the current task should end,
// based on s_task_anchor + duration. Cancels any previous one first.
static void schedule_wakeup_for_remaining(void) {
  cancel_wakeup();
  time_t now = time(NULL);
  uint32_t total = current_task()->duration;
  uint32_t elapsed = (now >= s_task_anchor) ? (uint32_t)(now - s_task_anchor) : 0;
  uint32_t remaining = (elapsed < total) ? (total - elapsed) : 0;

  if (remaining < WAKEUP_MIN_LEAD_SEC) {
    // Too close to the boundary to schedule reliably; the in-app tick
    // handler will catch it if we're open, and resync will catch up
    // next time the app launches either way.
    return;
  }

  time_t fire_time = now + remaining;
  WakeupId id = wakeup_schedule(fire_time, 0, true);
  s_wakeup_id = id;  // negative on failure; treated as "none" elsewhere
  persist_write_int(PERSIST_KEY_WAKEUP_ID, (int)s_wakeup_id);
}

// ============================================================
// TIMER RENDER
// ============================================================
static void render_time(void) {
  uint32_t m = s_remaining / 60;
  uint32_t s = s_remaining % 60;
  snprintf(s_time_buf, sizeof(s_time_buf), "%lu:%02lu", m, s);
  text_layer_set_text(s_time_layer, s_time_buf);
}

static void render_position(void) {
  snprintf(s_pos_buf, sizeof(s_pos_buf),
           "Task %d of %d", (int)s_current_task + 1, (int)active()->task_count);
  text_layer_set_text(s_position_layer, s_pos_buf);
}

static void render_status(void) {
  switch (s_state) {
    case STATE_IDLE:    text_layer_set_text(s_status_layer, "Press Select"); break;
    case STATE_RUNNING: text_layer_set_text(s_status_layer, "");             break;
    case STATE_PAUSED:  text_layer_set_text(s_status_layer, "Paused");       break;
    case STATE_DONE:    text_layer_set_text(s_status_layer, "");             break;
    case STATE_EMPTY:   text_layer_set_text(s_status_layer, "");             break;
  }
}

static void load_current_task_display(void) {
  Task *t = current_task();
  text_layer_set_text(s_name_layer, t->name);
  text_layer_set_background_color(s_name_layer, task_color(t->color));
  text_layer_set_text_color(s_name_layer, GColorWhite);
  text_layer_set_text(s_listname_layer, active()->name);
  render_time();
  render_position();
  if (s_progress_layer) layer_mark_dirty(s_progress_layer);
}

static void render_done_display(void) {
  text_layer_set_text(s_name_layer, "Done!");
  text_layer_set_background_color(s_name_layer, GColorIslamicGreen);
  text_layer_set_text_color(s_name_layer, GColorWhite);
  text_layer_set_text(s_time_layer, "");
  text_layer_set_text(s_listname_layer, active()->name);
  text_layer_set_text(s_position_layer, "");
  render_status();
  layer_mark_dirty(s_progress_layer);
}

static void render_empty_display(void) {
  text_layer_set_text(s_name_layer, "No lists");
  text_layer_set_background_color(s_name_layer, GColorDarkGray);
  text_layer_set_text_color(s_name_layer, GColorWhite);
  text_layer_set_text(s_time_layer, "");
  text_layer_set_text(s_status_layer, "Add one in");
  text_layer_set_text(s_position_layer, "phone settings");
  text_layer_set_text(s_listname_layer, "");
  layer_mark_dirty(s_progress_layer);
}

static void timer_render_all(void) {
  if (s_state == STATE_EMPTY) {
    render_empty_display();
  } else if (s_state == STATE_DONE) {
    render_done_display();
  } else {
    load_current_task_display();
    render_status();
  }
}

// ============================================================
// PROGRESS BAR
// ============================================================
static void progress_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (s_state == STATE_DONE) {
    graphics_context_set_fill_color(ctx, GColorIslamicGreen);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    return;
  }
  if (s_state == STATE_EMPTY) {
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    return;
  }
  uint32_t total = current_task()->duration;
  uint32_t elapsed = (total >= s_remaining) ? (total - s_remaining) : 0;
  int16_t fill_w = (int16_t)((b.size.w * elapsed) / total);

  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, task_color(current_task()->color));
  graphics_fill_rect(ctx, GRect(0, 0, fill_w, b.size.h), 0, GCornerNone);
}

// ============================================================
// DONE SCREEN
// ============================================================
static void done_tick(void *ctx) {
  if (s_state != STATE_DONE) return;
  s_done_timer = NULL;
  exit_done_state();
  show_picker();
}

static void enter_done_state(void) {
  tick_timer_service_unsubscribe();
  cancel_wakeup();  // no next task, nothing to schedule
  s_state = STATE_DONE;
  s_done_countdown = DONE_SCREEN_SEC;
  vibe_complete();
  render_done_display();
  s_done_timer = app_timer_register(DONE_SCREEN_SEC * 1000, done_tick, NULL);
  touch_activity();
  save_state();
}

static void exit_done_state(void) {
  if (s_done_timer) {
    app_timer_cancel(s_done_timer);
    s_done_timer = NULL;
  }
  s_state = STATE_IDLE;
  s_current_task = 0;
  s_remaining = current_task()->duration;
  save_state();
}

// ============================================================
// ADVANCE (foreground tick reaching zero)
// ============================================================
// Returns the list to jump to when the active list finishes, or -1.
static int chain_target(void) {
  uint8_t t = active()->then_run;
  if (t == 0) return -1;
  uint8_t idx = t - 1;
  if (idx >= s_library.list_count) return -1;
  if (idx == s_active_list) return -1;
  if (s_library.lists[idx].task_count == 0) return -1;
  return idx;
}

static void advance_task(void) {
  uint8_t next = s_current_task + 1;
  if (next >= active()->task_count) {
    if (active()->loops) {
      next = 0;
    } else {
      int chain = chain_target();
      if (chain < 0) {
        enter_done_state();
        return;
      }
      s_active_list = (uint8_t)chain;   // "then run": jump to next list
      next = 0;
    }
    s_current_task = next;
    s_task_anchor = time(NULL);
    s_remaining = current_task()->duration;
    load_current_task_display();
    vibe_advance();
    schedule_wakeup_for_remaining();
    touch_activity();
    save_state();
    return;
  }
  s_current_task = next;
  s_task_anchor = time(NULL);
  s_remaining = current_task()->duration;
  load_current_task_display();
  vibe_advance();
  schedule_wakeup_for_remaining();
  touch_activity();
  save_state();
}

static void tick_handler(struct tm *t, TimeUnits units) {
  if (s_state != STATE_RUNNING) return;
  time_t now = time(NULL);
  uint32_t total = current_task()->duration;
  uint32_t elapsed = (now >= s_task_anchor) ? (uint32_t)(now - s_task_anchor) : 0;

  if (elapsed >= total) {
    s_remaining = 0;
    render_time();
    layer_mark_dirty(s_progress_layer);
    advance_task();
    return;
  }

  s_remaining = total - elapsed;
  render_time();
  layer_mark_dirty(s_progress_layer);
  if (s_remaining % 5 == 0) save_state();
}

// ============================================================
// RESYNC ON LAUNCH
// Recomputes where we actually are, based on real elapsed time
// since the current task started. Hops through any boundaries
// crossed while the app was closed (bounded, and only reached
// when we're inside the RESUME_WINDOW_SEC staleness gate).
// ============================================================
static void resync_running_state(void) {
  if (s_state != STATE_RUNNING) return;
  if (s_library.list_count == 0) return;

  time_t now = time(NULL);
  uint32_t elapsed = (now >= s_task_anchor) ? (uint32_t)(now - s_task_anchor) : 0;
  bool advanced_any = false;
  int hops = 0;

  while (hops < MAX_RESYNC_HOPS) {
    uint32_t total = current_task()->duration;
    if (elapsed < total) break;  // landed within this task

    elapsed -= total;
    advanced_any = true;
    hops++;

    uint8_t next = s_current_task + 1;
    if (next >= active()->task_count) {
      if (active()->loops) {
        next = 0;
      } else {
        int chain = chain_target();
        if (chain < 0) {
          enter_done_state();
          return;
        }
        s_active_list = (uint8_t)chain;   // "then run": jump to next list
        next = 0;
      }
    }
    s_current_task = next;
  }

  s_task_anchor = now - elapsed;
  s_remaining = current_task()->duration - elapsed;

  load_current_task_display();
  if (advanced_any) {
    vibe_advance();
  }
  touch_activity();
  schedule_wakeup_for_remaining();
  save_state();
}

// Fires if the app happens to already be open when a wakeup lands.
static void wakeup_handler(WakeupId id, int32_t cookie) {
  resync_running_state();
}

// ============================================================
// ACTIVATE LIST (from picker)
// ============================================================
static void activate_list(uint8_t idx) {
  if (idx >= s_library.list_count) return;
  if (s_state == STATE_DONE) {
    exit_done_state();
  }
  cancel_wakeup();
  s_active_list = idx;
  s_current_task = 0;
  s_task_anchor = time(NULL);
  s_remaining = current_task()->duration;
  if (s_state != STATE_RUNNING) {
    s_state = STATE_RUNNING;
    tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  }
  schedule_wakeup_for_remaining();
  timer_render_all();
  touch_activity();
  save_state();
}

// ============================================================
// PICKER
// ============================================================
static uint16_t picker_num_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return s_library.list_count;
}

static void picker_draw_row(GContext *ctx, const Layer *cell_layer,
                             MenuIndex *idx, void *context) {
  TaskList *l = &s_library.lists[idx->row];
  char subtitle[28];
  uint32_t total_sec = 0;
  for (int i = 0; i < l->task_count; i++) total_sec += l->tasks[i].duration;
  const char *loop_marker = l->loops ? " >" : "";
  snprintf(subtitle, sizeof(subtitle), "%d tasks, %lu min%s",
           (int)l->task_count, total_sec / 60, loop_marker);
  menu_cell_basic_draw(ctx, cell_layer, l->name, subtitle, NULL);
}

static int16_t picker_cell_height(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return 44;
}

static void picker_select_click(MenuLayer *m, MenuIndex *idx, void *ctx) {
  activate_list(idx->row);
  hide_picker();
}

static void picker_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_picker_menu = menu_layer_create(b);
  menu_layer_set_callbacks(s_picker_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows        = picker_num_rows,
    .draw_row            = picker_draw_row,
    .get_cell_height     = picker_cell_height,
    .select_click        = picker_select_click,
  });
  menu_layer_set_click_config_onto_window(s_picker_menu, window);
  MenuIndex start = { .section = 0, .row = s_active_list };
  menu_layer_set_selected_index(s_picker_menu, start, MenuRowAlignCenter, false);
  layer_add_child(root, menu_layer_get_layer(s_picker_menu));
}

static void picker_window_unload(Window *window) {
  menu_layer_destroy(s_picker_menu);
  s_picker_menu = NULL;
}

static void show_picker(void) {
  if (s_picker_window) return;
  if (s_library.list_count == 0) return;
  s_picker_window = window_create();
  window_set_window_handlers(s_picker_window, (WindowHandlers){
    .load = picker_window_load,
    .unload = picker_window_unload,
  });
  window_stack_push(s_picker_window, true);
}

static void hide_picker(void) {
  if (!s_picker_window) return;
  window_stack_remove(s_picker_window, true);
  window_destroy(s_picker_window);
  s_picker_window = NULL;
}

// ============================================================
// TIMER WINDOW BUTTONS
// ============================================================
static void start_running(void) {
  // Backdate the anchor so (duration - (now - anchor)) == current s_remaining,
  // whether resuming from pause or starting a task fresh.
  s_task_anchor = time(NULL) - (int32_t)(current_task()->duration - s_remaining);
  s_state = STATE_RUNNING;
  render_status();
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  schedule_wakeup_for_remaining();
  touch_activity();
  save_state();
}

static void pause_running(void) {
  time_t now = time(NULL);
  uint32_t total = current_task()->duration;
  uint32_t elapsed = (now >= s_task_anchor) ? (uint32_t)(now - s_task_anchor) : 0;
  s_remaining = (elapsed < total) ? (total - elapsed) : 0;
  s_state = STATE_PAUSED;
  tick_timer_service_unsubscribe();
  cancel_wakeup();
  render_status();
  vibes_short_pulse();
  touch_activity();
  save_state();
}

static void select_click(ClickRecognizerRef r, void *c) {
  switch (s_state) {
    case STATE_IDLE:
    case STATE_PAUSED:  start_running(); break;
    case STATE_RUNNING: pause_running(); break;
    case STATE_DONE:
      exit_done_state();
      show_picker();
      break;
    default: break;
  }
}

static void select_long_click(ClickRecognizerRef r, void *c) {
  if (s_state == STATE_DONE || s_state == STATE_EMPTY) return;
  s_remaining = current_task()->duration;
  if (s_state == STATE_RUNNING) {
    s_task_anchor = time(NULL);
    schedule_wakeup_for_remaining();
  }
  render_time();
  layer_mark_dirty(s_progress_layer);
  vibes_short_pulse();
  touch_activity();
  save_state();
}

static void up_click(ClickRecognizerRef r, void *c) {
  if (s_state == STATE_EMPTY) return;
  if (s_state == STATE_DONE) {
    exit_done_state();
    show_picker();
    return;
  }
  uint8_t prev = (s_current_task == 0)
    ? active()->task_count - 1 : s_current_task - 1;
  s_current_task = prev;
  s_remaining = current_task()->duration;
  if (s_state == STATE_RUNNING) {
    s_task_anchor = time(NULL);
    schedule_wakeup_for_remaining();
  }
  load_current_task_display();
  vibes_short_pulse();
  touch_activity();
  save_state();
}

static void down_click(ClickRecognizerRef r, void *c) {
  if (s_state == STATE_EMPTY) return;
  if (s_state == STATE_DONE) {
    exit_done_state();
    show_picker();
    return;
  }
  uint8_t next = (s_current_task + 1) % active()->task_count;
  s_current_task = next;
  s_remaining = current_task()->duration;
  if (s_state == STATE_RUNNING) {
    s_task_anchor = time(NULL);
    schedule_wakeup_for_remaining();
  }
  load_current_task_display();
  vibes_short_pulse();
  touch_activity();
  save_state();
}

static void down_long_click(ClickRecognizerRef r, void *c) {
  if (s_state == STATE_DONE) {
    exit_done_state();
  }
  show_picker();
}

static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_multi_click_subscribe(BUTTON_ID_DOWN, 1, 1, 0, true, down_click);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long_click, NULL);
}

// ============================================================
// TIMER WINDOW LIFECYCLE
// ============================================================
static void timer_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  window_set_background_color(window, GColorWhite);

  s_name_layer = text_layer_create(GRect(0, 0, b.size.w, 44));
  text_layer_set_font(s_name_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_name_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_name_layer));

  s_time_layer = text_layer_create(GRect(0, 56, b.size.w, 76));
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_time_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  s_status_layer = text_layer_create(GRect(0, 140, b.size.w, 22));
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  s_progress_layer = layer_create(GRect(0, 168, b.size.w, 10));
  layer_set_update_proc(s_progress_layer, progress_update_proc);
  layer_add_child(root, s_progress_layer);

  s_position_layer = text_layer_create(GRect(0, 180, b.size.w, 22));
  text_layer_set_font(s_position_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_position_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_position_layer, GColorClear);
  text_layer_set_text_color(s_position_layer, GColorDarkGray);
  layer_add_child(root, text_layer_get_layer(s_position_layer));

  s_listname_layer = text_layer_create(GRect(0, 202, b.size.w, 22));
  text_layer_set_font(s_listname_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_listname_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_listname_layer, GColorClear);
  text_layer_set_text_color(s_listname_layer, GColorDarkGray);
  layer_add_child(root, text_layer_get_layer(s_listname_layer));

  timer_render_all();
}

static void timer_window_unload(Window *window) {
  if (s_done_timer) {
    app_timer_cancel(s_done_timer);
    s_done_timer = NULL;
  }
  text_layer_destroy(s_name_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_position_layer);
  text_layer_destroy(s_listname_layer);
  layer_destroy(s_progress_layer);
}

// ============================================================
// CONFIG RECEIVE (AppMessage from phone)
// ============================================================

static uint32_t parse_uint(const char *s, int len) {
  uint32_t v = 0;
  for (int i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') break;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

static void copy_field(char *dest, int dest_size, const char *src, int len) {
  int n = (len < dest_size - 1) ? len : dest_size - 1;
  for (int i = 0; i < n; i++) dest[i] = src[i];
  dest[n] = '\0';
}

static const char* find_delim(const char *p, const char *end, char delim) {
  while (p < end && *p != delim) p++;
  return p;
}

static void parse_task(const char *p, const char *end, Task *t) {
  const char *c1 = find_delim(p, end, '^');
  copy_field(t->name, MAX_TASK_NAME, p, c1 - p);

  const char *m_start = (c1 < end) ? c1 + 1 : end;
  const char *c2 = find_delim(m_start, end, '^');
  uint32_t minutes = parse_uint(m_start, c2 - m_start);
  if (minutes < 1) minutes = 1;
  if (minutes > 90) minutes = 90;
  t->duration = minutes * 60;

  const char *col_start = (c2 < end) ? c2 + 1 : end;
  uint32_t color = parse_uint(col_start, end - col_start);
  if (color >= PALETTE_SIZE) color = 0;
  t->color = (uint8_t)color;
}

static void parse_list(const char *p, const char *end, TaskList *l) {
  l->protected_flag = 0;
  l->then_run = 0;
  l->task_count = 0;

  const char *d = find_delim(p, end, '|');
  copy_field(l->name, MAX_LIST_NAME, p, d - p);
  if (d >= end) return;
  p = d + 1;

  d = find_delim(p, end, '|');
  l->loops = (parse_uint(p, d - p) != 0) ? 1 : 0;
  if (d >= end) return;
  p = d + 1;

  d = find_delim(p, end, '|');
  uint32_t declared = parse_uint(p, d - p);
  if (d >= end) return;
  p = d + 1;

  uint8_t count = 0;
  uint32_t seen = 0;
  while (p < end && seen < declared) {
    const char *tdelim = find_delim(p, end, '|');
    if (count < MAX_TASKS) {
      parse_task(p, tdelim, &l->tasks[count]);
      count++;
    }
    seen++;
    if (tdelim >= end) { p = end; break; }
    p = tdelim + 1;
  }
  l->task_count = count;

  // Optional trailing field: then_run (0 = none, else list index + 1).
  // Absent in older phone-side builds.
  if (p < end) {
    const char *d2 = find_delim(p, end, '|');
    uint32_t tr = parse_uint(p, d2 - p);
    l->then_run = (tr <= MAX_LISTS) ? (uint8_t)tr : 0;
  }
}

static char s_prev_active_name[MAX_LIST_NAME];

static void snapshot_active_name(void) {
  if (s_library.list_count > 0 && s_active_list < s_library.list_count) {
    strncpy(s_prev_active_name, s_library.lists[s_active_list].name, MAX_LIST_NAME - 1);
    s_prev_active_name[MAX_LIST_NAME - 1] = '\0';
  } else {
    s_prev_active_name[0] = '\0';
  }
}

static uint8_t find_list_by_name(const char *name) {
  if (!name || name[0] == '\0') return 0;
  for (uint8_t i = 0; i < s_library.list_count; i++) {
    if (strcmp(s_library.lists[i].name, name) == 0) return i;
  }
  return 0;
}

static void parse_library(const char *wire, int wire_len) {
  const char *p = wire;
  const char *end = wire + wire_len;

  const char *d = find_delim(p, end, '~');
  uint32_t declared = parse_uint(p, d - p);
  if (d >= end) {
    s_library.list_count = 0;
    return;
  }
  p = d + 1;

  uint8_t count = 0;
  while (p < end && count < MAX_LISTS && count < declared) {
    const char *ldelim = find_delim(p, end, '~');
    parse_list(p, ldelim, &s_library.lists[count]);
    count++;
    if (ldelim >= end) break;
    p = ldelim + 1;
  }
  s_library.list_count = count;
}

static void refresh_after_config(void) {
  cancel_wakeup();

  if (s_library.list_count == 0) {
    s_state = STATE_EMPTY;
    s_active_list = 0;
    s_current_task = 0;
    s_remaining = 0;
    tick_timer_service_unsubscribe();
    hide_picker();
    timer_render_all();
    save_all();
    return;
  }

  s_active_list = find_list_by_name(s_prev_active_name);
  s_current_task = 0;
  s_remaining = current_task()->duration;

  if (s_state == STATE_RUNNING) {
    tick_timer_service_unsubscribe();
    s_state = STATE_PAUSED;
  }
  if (s_state == STATE_EMPTY) {
    s_state = STATE_IDLE;
  }

  if (s_picker_window && s_picker_menu) {
    menu_layer_reload_data(s_picker_menu);
  } else {
    timer_render_all();
  }
  save_all();
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_LIBRARY_DATA);
  if (!t) return;
  const char *wire = t->value->cstring;
  int len = strlen(wire);

  snapshot_active_name();
  parse_library(wire, len);
  refresh_after_config();
}

// ============================================================
// APP INIT
// ============================================================
static void init(void) {
  load_all();

  // Clear any wakeup left over from before this launch; the flow
  // below will schedule a fresh one if warranted.
  if (s_wakeup_id >= 0) {
    wakeup_cancel(s_wakeup_id);
    s_wakeup_id = -1;
  }

  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);

  wakeup_service_subscribe(wakeup_handler);

  s_timer_window = window_create();
  window_set_click_config_provider(s_timer_window, click_config_provider);
  window_set_window_handlers(s_timer_window, (WindowHandlers){
    .load = timer_window_load,
    .unload = timer_window_unload
  });
  window_stack_push(s_timer_window, true);

  if (s_state == STATE_EMPTY) return;

  time_t now = time(NULL);
  bool stale = (s_last_activity == 0) ||
               (now - s_last_activity) >= RESUME_WINDOW_SEC;

  if (stale) {
    if (s_state == STATE_RUNNING) {
      uint32_t total = current_task()->duration;
      uint32_t elapsed = (now >= s_task_anchor) ? (uint32_t)(now - s_task_anchor) : 0;
      s_remaining = (elapsed < total) ? (total - elapsed) : 0;
      s_state = STATE_PAUSED;
      save_state();
    }
    timer_render_all();
    show_picker();
    return;
  }

  if (s_state == STATE_RUNNING) {
    tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
    resync_running_state();
  } else {
    timer_render_all();
  }
}

static void deinit(void) {
  if (s_done_timer) {
    app_timer_cancel(s_done_timer);
    s_done_timer = NULL;
  }
  save_state();
  if (s_picker_window) {
    window_destroy(s_picker_window);
  }
  window_destroy(s_timer_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
