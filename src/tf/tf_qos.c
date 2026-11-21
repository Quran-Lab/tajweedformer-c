/* 4.0.1 (perf_plan row 8), Windows: the engine's threads are never power-throttled. On a hybrid CPU Windows puts a
 * thread it judges to be background work (EcoQoS: the app minimised, behind other windows, or started without a
 * window) on the efficiency cores at a lower clock; the plan measured the same engine at 134 to 140 ms a chunk there
 * against 78 to 81 ms on a performance core, and an unpinned console process landed on the efficiency cores. A
 * thread whose power throttling is switched off (ThreadPowerThrottling, ControlMask EXECUTION_SPEED, StateMask 0:
 * "high QoS") is kept on the fast cores whatever the window does.
 *
 * Which threads: the Core's own engine threads, made inside qlc_model_open and qlc_stream_create (the process's
 * threads that were not there before those calls: tf__qos_snapshot then tf__qos_mark_new; a thread the app starts in
 * that moment is marked too, and only costs a little power), and the thread that pushes audio, for the length of
 * each push and finish (tf__qos_enter, tf__qos_leave put back what that thread had, since it is the app's).
 * The process's threads are walked with ntdll's NtGetNextThread, which visits this process's threads only: the
 * Toolhelp snapshot lists every thread of the machine and cost 0.4 s a call on a busy PC (each model open took 0.8 s
 * longer with it). Without NtGetNextThread no engine thread is marked; the pushing thread still is.
 * Nothing changes a result: the cores a thread runs on change its time, never a bit. Elsewhere these are no-ops; on
 * a Windows without the call (before Windows 10 1709) too. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <string.h>

#include "tf_internal.h"

#ifdef _WIN32
#include <windows.h>

/* processthreadsapi.h's names, here so an older SDK's headers build this file too */
#define TF_THREAD_POWER_THROTTLING 3 /* THREAD_INFORMATION_CLASS ThreadPowerThrottling */
#define TF_PT_VERSION 1              /* THREAD_POWER_THROTTLING_CURRENT_VERSION */
#define TF_PT_EXECUTION_SPEED 0x1    /* THREAD_POWER_THROTTLING_EXECUTION_SPEED */
typedef struct {
  ULONG Version, ControlMask, StateMask;
} TfPowerThrottling;
typedef BOOL(WINAPI *SetInfoFn)(HANDLE, int, LPVOID, DWORD);
typedef BOOL(WINAPI *GetInfoFn)(HANDLE, int, LPVOID, DWORD);
/* ntdll (Windows Vista and later): the thread of `process` after `thread` (NULL: the first), opened with `access` */
typedef LONG(NTAPI *NextThreadFn)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);

static SetInfoFn g_set;
static GetInfoFn g_get;
static NextThreadFn g_next;
static volatile LONG g_resolved;
static void resolve(void) {
  if (InterlockedCompareExchange(&g_resolved, 1, 0) != 0) return;
  HMODULE k = GetModuleHandleW(L"kernel32.dll"), nt = GetModuleHandleW(L"ntdll.dll");
  if (k) {
    g_set = (SetInfoFn)(void (*)(void))GetProcAddress(k, "SetThreadInformation");
    g_get = (GetInfoFn)(void (*)(void))GetProcAddress(k, "GetThreadInformation");
  }
  if (nt) g_next = (NextThreadFn)(void (*)(void))GetProcAddress(nt, "NtGetNextThread");
}
/* each thread of this process, opened with `access`, to fn (which must not close it); stops when fn answers nonzero */
static void each_thread(ACCESS_MASK access, int (*fn)(HANDLE h, DWORD id, void *user), void *user) {
  resolve();
  if (!g_next) return;
  HANDLE prev = NULL, h = NULL;
  int stop = 0;
  while (!stop && g_next(GetCurrentProcess(), prev, access, 0, 0, &h) == 0) {
    if (prev) CloseHandle(prev);
    prev = h;
    stop = fn(h, GetThreadId(h), user);
  }
  if (prev) CloseHandle(prev);
}
int tf__qos_off; /* tests and measurements: 1 leaves every thread as Windows has it (the 4.0.0 behaviour) */

/* high QoS for one thread: 0, or -1 when the system has no such call or refused it */
static int high_qos(HANDLE h) {
  resolve();
  if (!g_set || tf__qos_off) return -1;
  TfPowerThrottling s = {TF_PT_VERSION, TF_PT_EXECUTION_SPEED, 0};
  return g_set(h, TF_THREAD_POWER_THROTTLING, &s, sizeof s) ? 0 : -1;
}

static int add_id(HANDLE h, DWORD id, void *user) {
  (void)h;
  TfThreadSnap *s = (TfThreadSnap *)user;
  if (s->n < (int)(sizeof s->ids / sizeof s->ids[0])) s->ids[s->n++] = id;
  else s->full = 1;
  return 0;
}
void tf__qos_snapshot(TfThreadSnap *s) {
  memset(s, 0, sizeof *s);
  resolve();
  if (!g_next || tf__qos_off) return;
  each_thread(THREAD_QUERY_LIMITED_INFORMATION, add_id, s);
  s->taken = 1;
}

typedef struct {
  const TfThreadSnap *before;
  int marked;
} MarkNew;
static int mark_if_new(HANDLE h, DWORD id, void *user) {
  MarkNew *k = (MarkNew *)user;
  for (int j = 0; j < k->before->n; ++j)
    if (k->before->ids[j] == id) return 0;
  k->marked += high_qos(h) == 0;
  return 0;
}
int tf__qos_mark_new(const TfThreadSnap *before) {
  if (!before || !before->taken || before->full || tf__qos_off) return 0;
  MarkNew k = {before, 0};
  each_thread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, mark_if_new, &k);
  return k.marked;
}

void tf__qos_enter(TfQosSave *q) {
  memset(q, 0, sizeof *q);
  resolve();
  if (!g_set || !g_get || tf__qos_off) return;
  TfPowerThrottling was = {TF_PT_VERSION, 0, 0};
  if (!g_get(GetCurrentThread(), TF_THREAD_POWER_THROTTLING, &was, sizeof was)) return;
  if ((was.ControlMask & TF_PT_EXECUTION_SPEED) && !(was.StateMask & TF_PT_EXECUTION_SPEED)) return; /* already high QoS */
  if (high_qos(GetCurrentThread())) return;
  q->set = 1;
  q->control = was.ControlMask;
  q->state = was.StateMask;
}

void tf__qos_leave(const TfQosSave *q) {
  if (!q || !q->set || !g_set) return;
  TfPowerThrottling s = {TF_PT_VERSION, q->control, q->state};
  g_set(GetCurrentThread(), TF_THREAD_POWER_THROTTLING, &s, sizeof s);
}

/* tests: whether this thread runs with high QoS now (1), not (0), or the system cannot tell (-1) */
int tf__qos_thread_high(void) {
  resolve();
  if (!g_get) return -1;
  TfPowerThrottling s = {TF_PT_VERSION, 0, 0};
  if (!g_get(GetCurrentThread(), TF_THREAD_POWER_THROTTLING, &s, sizeof s)) return -1;
  return (s.ControlMask & TF_PT_EXECUTION_SPEED) && !(s.StateMask & TF_PT_EXECUTION_SPEED);
}
#else
int tf__qos_off;
void tf__qos_snapshot(TfThreadSnap *s) { memset(s, 0, sizeof *s); }
int tf__qos_mark_new(const TfThreadSnap *before) {
  (void)before;
  return 0;
}
void tf__qos_enter(TfQosSave *q) { memset(q, 0, sizeof *q); }
void tf__qos_leave(const TfQosSave *q) { (void)q; }
int tf__qos_thread_high(void) { return -1; }
#endif
