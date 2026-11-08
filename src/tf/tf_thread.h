/* A minimal thread, recursive mutex and condition variable for the sharing uploader (internal). */
#ifndef TF_THREAD_H
#define TF_THREAD_H

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION TfMutex; /* recursive */
typedef CONDITION_VARIABLE TfCond;
typedef HANDLE TfThread;
static inline void tf_mutex_init(TfMutex *m) { InitializeCriticalSection(m); }
static inline void tf_mutex_destroy(TfMutex *m) { DeleteCriticalSection(m); }
static inline void tf_mutex_lock(TfMutex *m) { EnterCriticalSection(m); }
static inline void tf_mutex_unlock(TfMutex *m) { LeaveCriticalSection(m); }
static inline void tf_cond_init(TfCond *c) { InitializeConditionVariable(c); }
static inline void tf_cond_destroy(TfCond *c) { (void)c; }
static inline void tf_cond_signal(TfCond *c) { WakeAllConditionVariable(c); }
/* waits at most ms; the mutex must be held (once) */
static inline void tf_cond_wait_ms(TfCond *c, TfMutex *m, long ms) { SleepConditionVariableCS(c, m, ms < 0 ? 0 : (DWORD)ms); }
typedef struct {
  void (*fn)(void *);
  void *arg;
} TfThreadStart__;
static DWORD WINAPI tf_thread_tramp__(LPVOID p) {
  TfThreadStart__ s = *(TfThreadStart__ *)p;
  HeapFree(GetProcessHeap(), 0, p);
  s.fn(s.arg);
  return 0;
}
static inline int tf_thread_start(TfThread *t, void (*fn)(void *), void *arg) {
  TfThreadStart__ *s = (TfThreadStart__ *)HeapAlloc(GetProcessHeap(), 0, sizeof *s);
  if (!s) return -1;
  s->fn = fn;
  s->arg = arg;
  *t = CreateThread(NULL, 0, tf_thread_tramp__, s, 0, NULL);
  if (!*t) HeapFree(GetProcessHeap(), 0, s);
  return *t ? 0 : -1;
}
static inline void tf_thread_join(TfThread t) {
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}
#else
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
typedef pthread_mutex_t TfMutex;
typedef pthread_cond_t TfCond;
typedef pthread_t TfThread;
static inline void tf_mutex_init(TfMutex *m) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &a);
  pthread_mutexattr_destroy(&a);
}
static inline void tf_mutex_destroy(TfMutex *m) { pthread_mutex_destroy(m); }
static inline void tf_mutex_lock(TfMutex *m) { pthread_mutex_lock(m); }
static inline void tf_mutex_unlock(TfMutex *m) { pthread_mutex_unlock(m); }
static inline void tf_cond_init(TfCond *c) { pthread_cond_init(c, NULL); }
static inline void tf_cond_destroy(TfCond *c) { pthread_cond_destroy(c); }
static inline void tf_cond_signal(TfCond *c) { pthread_cond_broadcast(c); }
static inline void tf_cond_wait_ms(TfCond *c, TfMutex *m, long ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  if (ms < 0) ms = 0;
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_cond_timedwait(c, m, &ts);
}
typedef struct {
  void (*fn)(void *);
  void *arg;
} TfThreadStart__;
static void *tf_thread_tramp__(void *p) {
  TfThreadStart__ s = *(TfThreadStart__ *)p;
  free(p);
  s.fn(s.arg);
  return NULL;
}
static inline int tf_thread_start(TfThread *t, void (*fn)(void *), void *arg) {
  TfThreadStart__ *s = (TfThreadStart__ *)malloc(sizeof *s);
  if (!s) return -1;
  s->fn = fn;
  s->arg = arg;
  if (pthread_create(t, NULL, tf_thread_tramp__, s)) {
    free(s);
    return -1;
  }
  return 0;
}
static inline void tf_thread_join(TfThread t) { pthread_join(t, NULL); }
#endif
#endif
