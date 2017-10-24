
#define _GNU_SOURCE
#include <dlfcn.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>

#include <unistd.h>

#include <pthread.h>

#include "dcmalloc.h"

#define DCMALLOC_DEBUG

#ifdef DCMALLOC_DEBUG
#define PRINT_DEBUG(STR, ...) \
    fprintf(stdout, "%s:%d %s " STR "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__);

#else
// #define PRINT_DEBUG(str, ...)
#endif

#define PRINT_ERR(STR, ...) \
    fprintf(stderr, "%s:%d %s " STR "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)

/*
 * Begin dc_malloc global data
 */

#define MAX_HISTOGRAM_ALLOC 100000
#define DCMALLOC_OUTPUT_FILE ".dcmalloc.dat"

// store allocation histogram
// each thread tracks allocation data in thread-local storage
// and then synchronizes it to global data on thread exit
// can only track exact allocation size up to a number
// larger allocations are counted on the largest histogram value
static _Thread_local size_t alloc_sizes[MAX_HISTOGRAM_ALLOC];
static size_t global_alloc_sizes[MAX_HISTOGRAM_ALLOC];
static pthread_mutex_t global_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;

/*
 * Begin dc_malloc functions.
 */

void dc_malloc_initialize()
{
    if (initialized)
        return;

    initialized = 1;
    PRINT_DEBUG();
    // printf("dc_malloc_initialize\n");
    if (atexit(dc_malloc_finalize) != 0)
        PRINT_ERR("cannot set exit function");
        // printf("dc_malloc_initialize - cannot set exit function\n");
}

void dc_malloc_finalize()
{
    if (!initialized)
        return;

    initialized = 0;
    PRINT_DEBUG();
    // printf("dc_malloc_finalize\n");

    FILE* f = fopen(DCMALLOC_OUTPUT_FILE, "w");
    if (!f)
    {
        PRINT_ERR("failed to open output file");
        return;
    }

    PRINT_DEBUG("Dumping histogram data...");
    for (uint32_t i = 0; i < MAX_HISTOGRAM_ALLOC; ++i)
    {
        if (global_alloc_sizes[i] > 0)
            fprintf(f, "Alloc size %u: %zu mallocs\n", i, global_alloc_sizes[i]);
    }
}

void dc_malloc_thread_initialize()
{
    PRINT_DEBUG();
}

void dc_malloc_thread_finalize()
{
    PRINT_DEBUG();

    {
        pthread_mutex_lock(&global_alloc_mutex);
        for (uint32_t i = 0; i < MAX_HISTOGRAM_ALLOC; ++i)
            global_alloc_sizes[i] += alloc_sizes[i];

        pthread_mutex_unlock(&global_alloc_mutex);
    }
}

void* dc_malloc(size_t size)
{
    // printf("dc_malloc(%zu)\n", size);

    if (size < MAX_HISTOGRAM_ALLOC)
        ++alloc_sizes[size];
    else
        ++alloc_sizes[MAX_HISTOGRAM_ALLOC - 1];

    static void* (*libc_malloc)(size_t) = NULL;
    if (libc_malloc == NULL)
        libc_malloc = dlsym(RTLD_NEXT, "malloc");

    return libc_malloc(size);
}

void dc_free(void* ptr)
{
    // printf("dc_free(%p)\n", ptr);

    static void (*libc_free)(void*) = NULL;
    if (libc_free == NULL)
        libc_free = dlsym(RTLD_NEXT, "free");

    return libc_free(ptr);
}

/*
 * End dcmalloc functions.
 */

/*
 * Begin externals hooks to dcmalloc
 */

// handle process init/exit hooks
static pthread_key_t destructor_key;
static int is_initialized = 0;

static void* thread_initializer(void*);
static void thread_finalizer(void*);

static DCMALLOC_ATTR(constructor)
void initializer()
{
    if (!is_initialized)
    {
        is_initialized = 1;
        pthread_key_create(&destructor_key, thread_finalizer);
        dc_malloc_initialize();
    }

    dc_malloc_thread_initialize();
}

static DCMALLOC_ATTR(destructor)
void finalizer()
{
    dc_malloc_thread_finalize();

    if (is_initialized)
    {
        is_initialized = 0;
        dc_malloc_finalize();
    }
}

// handle thread init/exit hooks
typedef struct
{
    void* (*real_start)(void*);
    void* real_arg;
} thread_starter_arg;

static void* thread_initializer(void* argptr)
{
    thread_starter_arg* arg = (thread_starter_arg*)argptr;
    void* (*real_start)(void*) = arg->real_start;
    void* real_arg = arg->real_arg;
    dc_malloc_thread_initialize();

    pthread_setspecific(destructor_key, (void*)1);
    return (*real_start)(real_arg);
}

static void thread_finalizer(void* value)
{
    dc_malloc_thread_finalize();
}

int pthread_create(pthread_t* thread,
                   pthread_attr_t const* attr,
                   void* (start_routine)(void*),
                   void* arg)
{
    static int (*pthread_create_fn)(pthread_t*,
                                    pthread_attr_t const*,
                                    void* (void*),
                                    void*) = NULL;
    if (pthread_create_fn == NULL)
        pthread_create_fn = dlsym(RTLD_NEXT, "pthread_create");

    // @todo: don't want to use malloc here
    // instead using a ringbuffer, which has limited storage
#define RING_BUFFER_SIZE 10000
    static uint32_t _Atomic ring_buffer_pos = 0;
    static thread_starter_arg ring_buffer[RING_BUFFER_SIZE];
    uint32_t buffer_pos = atomic_fetch_add_explicit(&ring_buffer_pos, 1, memory_order_relaxed);

    thread_starter_arg* starter_arg = &ring_buffer[buffer_pos];
    starter_arg->real_start = start_routine;
    starter_arg->real_arg = arg;
    dc_malloc_thread_initialize();
    return pthread_create_fn(thread, attr, thread_initializer, starter_arg);
}

extern void* malloc(size_t size);
extern void free(void* ptr);

void* malloc(size_t size)
{
    return dc_malloc(size);
}

void free(void* ptr)
{
    return dc_free(ptr);
}

// glibc hooks
// overriding malloc/free turns out to be simpler than using glibc hooks
// DCMALLOC_EXPORT void (*__free_hook)(void *ptr) = dc_free;
// DCMALLOC_EXPORT void *(*__malloc_hook)(size_t size) = dc_malloc;






