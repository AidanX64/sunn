#ifndef FORGE_THREAD_H
#define FORGE_THREAD_H

#include <stddef.h>

typedef void (*ForgeThreadFunction)(void *argument);

/* Opaque handle for a started thread. */
typedef struct ForgeThread {
    void *handle;
} ForgeThread;

/* Recursive-free exclusive lock used to serialize shared logging. */
typedef struct ForgeMutex {
    void *handle;
} ForgeMutex;

int forge_thread_spawn(ForgeThread *thread, ForgeThreadFunction function, void *argument);
int forge_thread_join(ForgeThread *thread);

/* Recommended number of worker threads (logical processors; at least 1). */
int forge_thread_processor_count(void);

/* Initializes `mutex`; returns 0 on success and -1 when the lock could not
 * be created. Callers must treat a failed init as fatal for the operation:
 * lock/unlock on a dead mutex silently no-op, which would unsynchronize
 * workers exactly when resources are scarce. */
int forge_mutex_init(ForgeMutex *mutex);
void forge_mutex_destroy(ForgeMutex *mutex);
void forge_mutex_lock(ForgeMutex *mutex);
void forge_mutex_unlock(ForgeMutex *mutex);

#endif