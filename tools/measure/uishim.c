// t7 LD_PRELOAD shim for op1-og: UI frame time and key latency without editing the UI (t6's code).
//
//   LD_PRELOAD=tools/measure/uishim.so OP1_SHIM_OUT=prefix ./build/op1-og ...
//
// Hooks SDL_RenderPresent, SDL_UpdateTexture, SDL_PollEvent and (t10) SDL_WaitEventTimeout:
//   frames: <prefix>-frames.csv   t_ms, interval_us (present to present), cpu_us (UI thread CPU time between
//                                 presents = draw + texture upload + submit)
//   keys:   <prefix>-keys.csv     kind, poll_us (key pushed -> SDL_PollEvent returned it: the wait for the UI
//                                 loop), display_us (key pushed -> the Present that shows changed pixels)
// After OP1_SHIM_START seconds (default 6) a thread injects keys with SDL_PushEvent every 1.5 s:
//   '3' (tape screen, display changes), '1' (synth screen, display changes), 'a' down/up (note: poll delay only).
// Display time here is until SDL_RenderPresent returns, i.e. the app's last step; compositor and panel scan-out come on top.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ns(clockid_t c) {
  struct timespec ts;
  clock_gettime(c, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#define MAXF 400000
static float f_t[MAXF], f_int[MAXF], f_cpu[MAXF];
static int nf;
static int64_t last_present = 0, last_cpu = 0, t_first = 0;

#define MAXK 4000
static char k_kind[MAXK];
static float k_poll[MAXK], k_disp[MAXK];
static int nk;

static _Atomic int64_t push_t;      // CLOCK_MONOTONIC of the pending injected key, 0 = none
static _Atomic int64_t poll_t;      // when SDL_PollEvent returned it
static _Atomic uint64_t base_hash;  // display hash at push time
static _Atomic int want_display;    // 1: waiting for pixels to change
static _Atomic char cur_kind;
static _Atomic uint64_t last_hash;
static _Atomic int watch;           // hash texture uploads while injecting

static int active = 1;
static const uint32_t kMagic = 0x7e57;  // SDL_KeyboardEvent.which of our events

static uint64_t hash_pixels(const void* p, size_t n) {
  const uint64_t* w = (const uint64_t*)p;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n / 8; ++i) { h ^= w[i]; h *= 1099511628211ull; }
  return h;
}

bool SDL_UpdateTexture(SDL_Texture* tex, const SDL_Rect* rect, const void* pixels, int pitch) {
  static bool (*real)(SDL_Texture*, const SDL_Rect*, const void*, int);
  if (!real) real = dlsym(RTLD_NEXT, "SDL_UpdateTexture");
  if (atomic_load(&watch) && !rect) {
    float w = 0, h = 0;
    SDL_GetTextureSize(tex, &w, &h);
    atomic_store(&last_hash, hash_pixels(pixels, (size_t)pitch * (size_t)h));
  }
  return real(tex, rect, pixels, pitch);
}

bool SDL_RenderPresent(SDL_Renderer* r) {
  static bool (*real)(SDL_Renderer*);
  if (!real) real = dlsym(RTLD_NEXT, "SDL_RenderPresent");
  const bool ok = real(r);
  const int64_t t = now_ns(CLOCK_MONOTONIC), c = now_ns(CLOCK_THREAD_CPUTIME_ID);
  if (!t_first) t_first = t;
  if (last_present && nf < MAXF) {
    f_t[nf] = (float)((t - t_first) / 1e6);
    f_int[nf] = (float)((t - last_present) / 1e3);
    f_cpu[nf] = (float)((c - last_cpu) / 1e3);
    ++nf;
  }
  last_present = t;
  last_cpu = c;
  if (atomic_load(&want_display) && atomic_load(&last_hash) != atomic_load(&base_hash) && atomic_load(&poll_t)) {
    const int64_t p = atomic_load(&push_t);
    if (nk < MAXK) {
      k_kind[nk] = atomic_load(&cur_kind);
      k_poll[nk] = (float)((atomic_load(&poll_t) - p) / 1e3);
      k_disp[nk] = (float)((t - p) / 1e3);
      ++nk;
    }
    atomic_store(&want_display, 0);
    atomic_store(&push_t, 0);
    atomic_store(&poll_t, 0);
  }
  return ok;
}

static void got_event(const SDL_Event* e) {
  if (e && e->type == SDL_EVENT_KEY_DOWN && e->key.which == kMagic && atomic_load(&push_t) && !atomic_load(&poll_t)) {
    atomic_store(&poll_t, now_ns(CLOCK_MONOTONIC));
    if (!atomic_load(&want_display) && atomic_load(&cur_kind) == 'a') {  // poll delay only
      if (nk < MAXK) { k_kind[nk] = 'a'; k_poll[nk] = (float)((atomic_load(&poll_t) - atomic_load(&push_t)) / 1e3); k_disp[nk] = -1; ++nk; }
      atomic_store(&push_t, 0);
      atomic_store(&poll_t, 0);
    }
  }
}

bool SDL_PollEvent(SDL_Event* e) {
  static bool (*real)(SDL_Event*);
  if (!real) real = dlsym(RTLD_NEXT, "SDL_PollEvent");
  const bool got = real(e);
  if (got) got_event(e);
  return got;
}

// t10: the UI loop now sleeps in SDL_WaitEventTimeout and handles events as they arrive; same measurement.
bool SDL_WaitEventTimeout(SDL_Event* e, Sint32 ms) {
  static bool (*real)(SDL_Event*, Sint32);
  if (!real) real = dlsym(RTLD_NEXT, "SDL_WaitEventTimeout");
  const bool got = real(e, ms);
  if (got) got_event(e);
  return got;
}

static void push_key(SDL_Keycode k, bool down, bool measure, char kind, bool display) {
  SDL_Event e;
  SDL_zero(e);
  e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  e.key.which = kMagic;
  e.key.key = k;
  e.key.down = down;
  e.key.windowID = 0;
  if (measure) {
    atomic_store(&cur_kind, kind);
    atomic_store(&base_hash, atomic_load(&last_hash));
    atomic_store(&poll_t, 0);
    atomic_store(&want_display, display ? 1 : 0);
    atomic_store(&push_t, now_ns(CLOCK_MONOTONIC));
  }
  SDL_PushEvent(&e);
}

static void* injector(void* arg) {
  (void)arg;
  const char* s = getenv("OP1_SHIM_START");
  sleep((unsigned)(s ? atoi(s) : 6));
  atomic_store(&watch, 1);
  usleep(300000);
  for (int i = 0;; ++i) {
    const int step = i % 3;
    if (step == 0) { push_key(SDLK_3, true, true, '3', true); usleep(20000); push_key(SDLK_3, false, false, 0, false); }
    else if (step == 1) { push_key(SDLK_1, true, true, '1', true); usleep(20000); push_key(SDLK_1, false, false, 0, false); }
    else { push_key(SDLK_A, true, true, 'a', false); usleep(20000); push_key(SDLK_A, false, false, 0, false); }
    usleep(1500000 + (rand() % 7000));  // spread against the 16.7 ms frame grid
  }
  return NULL;
}

__attribute__((constructor)) static void shim_init(void) {
  char comm[64] = "";
  FILE* c = fopen("/proc/self/comm", "r");
  if (c) { if (!fgets(comm, sizeof comm, c)) comm[0] = 0; fclose(c); }
  if (strncmp(comm, "op1-og", 6) != 0) { active = 0; return; }  // run.sh, bash, pgrep ... also load the preload
  pthread_t t;
  pthread_create(&t, NULL, injector, NULL);
  pthread_detach(t);
}

__attribute__((destructor)) static void shim_done(void) {
  const char* p = getenv("OP1_SHIM_OUT");
  if (!p || !active) return;
  char path[1024];
  snprintf(path, sizeof path, "%s-frames.csv", p);
  FILE* f = fopen(path, "w");
  if (f) { fprintf(f, "t_ms,interval_us,cpu_us\n"); for (int i = 0; i < nf; ++i) fprintf(f, "%.3f,%.1f,%.1f\n", f_t[i], f_int[i], f_cpu[i]); fclose(f); }
  snprintf(path, sizeof path, "%s-keys.csv", p);
  f = fopen(path, "w");
  if (f) { fprintf(f, "kind,poll_us,display_us\n"); for (int i = 0; i < nk; ++i) fprintf(f, "%c,%.1f,%.1f\n", k_kind[i], k_poll[i], k_disp[i]); fclose(f); }
}
