#include <stdlib.h>

#include "forge/platform.h"
#include "forge/thread.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct ForgeThreadContext {
    ForgeThreadFunction function;
    void *argument;
} ForgeThreadContext;

static DWORD WINAPI thread_trampoline(LPVOID parameter)
{
    ForgeThreadContext *context = (ForgeThreadContext *)parameter;

    context->function(context->argument);
    free(context);
    return 0U;
}

int forge_thread_spawn(ForgeThread *thread, ForgeThreadFunction function, void *argument)
{
    ForgeThreadContext *context;
    HANDLE handle;

    if (thread == NULL || function == NULL) {
        return -1;
    }
    context = (ForgeThreadContext *)malloc(sizeof(*context));
    if (context == NULL) {
        return -1;
    }
    context->function = function;
    context->argument = argument;
    handle = CreateThread(NULL, 0, thread_trampoline, context, 0, NULL);
    if (handle == NULL) {
        free(context);
        return -1;
    }
    thread->handle = handle;
    return 0;
}

int forge_thread_join(ForgeThread *thread)
{
    if (thread == NULL || thread->handle == NULL) {
        return -1;
    }
    (void)WaitForSingleObject((HANDLE)thread->handle, INFINITE);
    (void)CloseHandle((HANDLE)thread->handle);
    thread->handle = NULL;
    return 0;
}

int forge_thread_processor_count(void)
{
    SYSTEM_INFO info;
    DWORD count;

    GetSystemInfo(&info);
    count = info.dwNumberOfProcessors;
    return count < 1U ? 1 : (int)count;
}

int forge_mutex_init(ForgeMutex *mutex)
{
    SRWLOCK *lock;

    if (mutex == NULL) {
        return -1;
    }
    lock = (SRWLOCK *)malloc(sizeof(*lock));
    if (lock == NULL) {
        mutex->handle = NULL;
        return -1;
    }
    InitializeSRWLock(lock);
    mutex->handle = lock;
    return 0;
}

void forge_mutex_destroy(ForgeMutex *mutex)
{
    if (mutex == NULL) {
        return;
    }
    free(mutex->handle);
    mutex->handle = NULL;
}

void forge_mutex_lock(ForgeMutex *mutex)
{
    if (mutex != NULL && mutex->handle != NULL) {
        AcquireSRWLockExclusive((SRWLOCK *)mutex->handle);
    }
}

void forge_mutex_unlock(ForgeMutex *mutex)
{
    if (mutex != NULL && mutex->handle != NULL) {
        ReleaseSRWLockExclusive((SRWLOCK *)mutex->handle);
    }
}
#else
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

typedef struct ForgeThreadContext {
    ForgeThreadFunction function;
    void *argument;
} ForgeThreadContext;

static void *thread_trampoline(void *parameter)
{
    ForgeThreadContext *context = (ForgeThreadContext *)parameter;

    context->function(context->argument);
    free(context);
    return NULL;
}

int forge_thread_spawn(ForgeThread *thread, ForgeThreadFunction function, void *argument)
{
    ForgeThreadContext *context;
    pthread_t handle;

    if (thread == NULL || function == NULL) {
        return -1;
    }
    context = (ForgeThreadContext *)malloc(sizeof(*context));
    if (context == NULL) {
        return -1;
    }
    context->function = function;
    context->argument = argument;
    if (pthread_create(&handle, NULL, thread_trampoline, context) != 0) {
        free(context);
        return -1;
    }
    thread->handle = (void *)handle;
    return 0;
}

int forge_thread_join(ForgeThread *thread)
{
    if (thread == NULL || thread->handle == NULL) {
        return -1;
    }
    (void)pthread_join((pthread_t)(uintptr_t)thread->handle, NULL);
    thread->handle = NULL;
    return 0;
}

int forge_thread_processor_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);

    return count < 1L ? 1 : (int)count;
}

int forge_mutex_init(ForgeMutex *mutex)
{
    pthread_mutex_t *lock;

    if (mutex == NULL) {
        return -1;
    }
    mutex->handle = NULL;
    lock = (pthread_mutex_t *)malloc(sizeof(*lock));
    if (lock == NULL) {
        return -1;
    }
    if (pthread_mutex_init(lock, NULL) != 0) {
        free(lock);
        return -1;
    }
    mutex->handle = lock;
    return 0;
}

void forge_mutex_destroy(ForgeMutex *mutex)
{
    if (mutex == NULL) {
        return;
    }
    if (mutex->handle != NULL) {
        (void)pthread_mutex_destroy((pthread_mutex_t *)mutex->handle);
    }
    free(mutex->handle);
    mutex->handle = NULL;
}

void forge_mutex_lock(ForgeMutex *mutex)
{
    if (mutex != NULL && mutex->handle != NULL) {
        (void)pthread_mutex_lock((pthread_mutex_t *)mutex->handle);
    }
}

void forge_mutex_unlock(ForgeMutex *mutex)
{
    if (mutex != NULL && mutex->handle != NULL) {
        (void)pthread_mutex_unlock((pthread_mutex_t *)mutex->handle);
    }
}
#endif