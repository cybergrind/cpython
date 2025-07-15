#ifndef Py_INTERNAL_CORO_METRICS_H
#define Py_INTERNAL_CORO_METRICS_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_time.h"   // PyTime_t

/* Forward declaration */
typedef struct _PyInterpreterFrame _PyInterpreterFrame;

/* Maximum number of execution chunks to track per coroutine */
#define CORO_MAX_CHUNKS 20  /* Keep only top 20 longest chunks */

typedef struct {
    PyTime_t start_time;    /* Start time of the chunk */
    PyTime_t duration;      /* Duration of the chunk in nanoseconds */
    PyObject *awaited_name; /* Name of the awaited function (owned ref) */
    PyObject *filename;     /* File path where await happened (owned ref) */
    int lineno;             /* Line number where await happened */
} CoroChunkMetric;

typedef struct {
    int chunk_count;        /* Number of chunks executed */
    int chunk_capacity;     /* Current capacity of chunks array */
    CoroChunkMetric *chunks; /* Dynamic array of chunk metrics */
    PyTime_t total_time;    /* Total execution time */
    int is_tracking;        /* Whether we're currently tracking this coroutine */
    PyTime_t current_chunk_start; /* Start time of current chunk */
} CoroMetrics;

/* Initialize coroutine metrics system */
extern void _PyCoroMetrics_Init(void);

/* Cleanup coroutine metrics system */
extern void _PyCoroMetrics_Fini(void);

/* Get or create metrics for a coroutine */
extern CoroMetrics* _PyCoroMetrics_Get(PyObject *coro);

/* Start tracking a new chunk */
extern void _PyCoroMetrics_StartChunk(PyObject *coro);

/* End current chunk tracking */
extern void _PyCoroMetrics_EndChunk(PyObject *coro, _PyInterpreterFrame *frame);

/* Free metrics associated with a coroutine */
extern void _PyCoroMetrics_Free(PyObject *coro);

/* Get metrics as a Python object */
extern PyObject* _PyCoroMetrics_GetMetrics(PyObject *coro);

/* Get all coroutine metrics globally */
extern PyObject* _PyCoroMetrics_GetAllMetrics(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CORO_METRICS_H */