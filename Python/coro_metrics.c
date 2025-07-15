#include "Python.h"
#include "pycore_coro_metrics.h"
#include "pycore_genobject.h"
#include "pycore_time.h"
#include <string.h>

/* Global dictionary to store coroutine -> metrics mapping */
static PyObject *coro_metrics_dict = NULL;

void
_PyCoroMetrics_Init(void)
{
    if (coro_metrics_dict == NULL) {
        coro_metrics_dict = PyDict_New();
    }
}

void
_PyCoroMetrics_Fini(void)
{
    if (coro_metrics_dict != NULL) {
        /* Clean up all metrics before clearing the dict */
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        
        while (PyDict_Next(coro_metrics_dict, &pos, &key, &value)) {
            CoroMetrics *metrics = (CoroMetrics *)PyLong_AsVoidPtr(value);
            if (metrics && metrics->chunks) {
                PyMem_Free(metrics->chunks);
            }
            if (metrics) {
                PyMem_Free(metrics);
            }
        }
        
        Py_CLEAR(coro_metrics_dict);
    }
}

CoroMetrics*
_PyCoroMetrics_Get(PyObject *coro)
{
    if (!PyCoro_CheckExact(coro)) {
        return NULL;
    }
    
    /* Initialize on first use */
    if (coro_metrics_dict == NULL) {
        _PyCoroMetrics_Init();
        if (coro_metrics_dict == NULL) {
            return NULL;
        }
    }
    
    PyObject *key = PyLong_FromVoidPtr(coro);
    if (key == NULL) {
        return NULL;
    }
    
    PyObject *value = PyDict_GetItem(coro_metrics_dict, key);
    CoroMetrics *metrics = NULL;
    
    if (value == NULL) {
        /* Create new metrics for this coroutine */
        metrics = PyMem_Calloc(1, sizeof(CoroMetrics));
        if (metrics == NULL) {
            Py_DECREF(key);
            PyErr_NoMemory();
            return NULL;
        }
        
        metrics->chunk_capacity = 16; /* Initial capacity */
        metrics->chunks = PyMem_Calloc(metrics->chunk_capacity, sizeof(CoroChunkMetric));
        if (metrics->chunks == NULL) {
            PyMem_Free(metrics);
            Py_DECREF(key);
            PyErr_NoMemory();
            return NULL;
        }
        
        PyObject *metrics_ptr = PyLong_FromVoidPtr(metrics);
        if (metrics_ptr == NULL) {
            PyMem_Free(metrics->chunks);
            PyMem_Free(metrics);
            Py_DECREF(key);
            return NULL;
        }
        
        if (PyDict_SetItem(coro_metrics_dict, key, metrics_ptr) < 0) {
            PyMem_Free(metrics->chunks);
            PyMem_Free(metrics);
            Py_DECREF(metrics_ptr);
            Py_DECREF(key);
            return NULL;
        }
        Py_DECREF(metrics_ptr);
    } else {
        metrics = (CoroMetrics *)PyLong_AsVoidPtr(value);
    }
    
    Py_DECREF(key);
    return metrics;
}

void
_PyCoroMetrics_StartChunk(PyObject *coro)
{
    CoroMetrics *metrics = _PyCoroMetrics_Get(coro);
    if (metrics == NULL) {
        return;
    }
    
    if (PyTime_PerfCounter(&metrics->current_chunk_start) < 0) {
        PyErr_Clear();
        return;
    }
    metrics->is_tracking = 1;
}

void
_PyCoroMetrics_EndChunk(PyObject *coro)
{
    CoroMetrics *metrics = _PyCoroMetrics_Get(coro);
    if (metrics == NULL || !metrics->is_tracking) {
        return;
    }
    
    PyTime_t end_time;
    if (PyTime_PerfCounter(&end_time) < 0) {
        PyErr_Clear();
        metrics->is_tracking = 0;
        return;
    }
    PyTime_t duration = end_time - metrics->current_chunk_start;
    
    /* Check if we need to expand the chunks array */
    if (metrics->chunk_count >= metrics->chunk_capacity) {
        if (metrics->chunk_count >= CORO_MAX_CHUNKS) {
            /* Don't track more than max chunks */
            metrics->is_tracking = 0;
            return;
        }
        
        int new_capacity = metrics->chunk_capacity * 2;
        if (new_capacity > CORO_MAX_CHUNKS) {
            new_capacity = CORO_MAX_CHUNKS;
        }
        
        CoroChunkMetric *new_chunks = PyMem_Realloc(metrics->chunks, 
                                                    new_capacity * sizeof(CoroChunkMetric));
        if (new_chunks == NULL) {
            /* Can't expand, stop tracking */
            metrics->is_tracking = 0;
            return;
        }
        
        metrics->chunks = new_chunks;
        metrics->chunk_capacity = new_capacity;
    }
    
    /* Record the chunk */
    metrics->chunks[metrics->chunk_count].start_time = metrics->current_chunk_start;
    metrics->chunks[metrics->chunk_count].duration = duration;
    metrics->chunk_count++;
    metrics->total_time += duration;
    metrics->is_tracking = 0;
}

void
_PyCoroMetrics_Free(PyObject *coro)
{
    if (coro_metrics_dict == NULL) {
        return;
    }
    
    PyObject *key = PyLong_FromVoidPtr(coro);
    if (key == NULL) {
        PyErr_Clear();
        return;
    }
    
    PyObject *value = PyDict_GetItem(coro_metrics_dict, key);
    if (value != NULL) {
        CoroMetrics *metrics = (CoroMetrics *)PyLong_AsVoidPtr(value);
        if (metrics) {
            if (metrics->chunks) {
                PyMem_Free(metrics->chunks);
            }
            PyMem_Free(metrics);
        }
        PyDict_DelItem(coro_metrics_dict, key);
    }
    
    Py_DECREF(key);
}

PyObject*
_PyCoroMetrics_GetMetrics(PyObject *coro)
{
    CoroMetrics *metrics = _PyCoroMetrics_Get(coro);
    if (metrics == NULL) {
        Py_RETURN_NONE;
    }
    
    PyObject *result = PyDict_New();
    if (result == NULL) {
        return NULL;
    }
    
    /* Add total time in seconds */
    double total_seconds = PyTime_AsSecondsDouble(metrics->total_time);
    PyObject *total_time = PyFloat_FromDouble(total_seconds);
    if (total_time == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    PyDict_SetItemString(result, "total_time", total_time);
    Py_DECREF(total_time);
    
    /* Add chunk count */
    PyObject *chunk_count = PyLong_FromLong(metrics->chunk_count);
    if (chunk_count == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    PyDict_SetItemString(result, "chunk_count", chunk_count);
    Py_DECREF(chunk_count);
    
    /* Add chunks list */
    PyObject *chunks = PyList_New(metrics->chunk_count);
    if (chunks == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    
    for (int i = 0; i < metrics->chunk_count; i++) {
        PyObject *chunk_dict = PyDict_New();
        if (chunk_dict == NULL) {
            Py_DECREF(chunks);
            Py_DECREF(result);
            return NULL;
        }
        
        double duration_seconds = PyTime_AsSecondsDouble(metrics->chunks[i].duration);
        PyObject *duration = PyFloat_FromDouble(duration_seconds);
        if (duration == NULL) {
            Py_DECREF(chunk_dict);
            Py_DECREF(chunks);
            Py_DECREF(result);
            return NULL;
        }
        
        PyDict_SetItemString(chunk_dict, "duration", duration);
        Py_DECREF(duration);
        
        PyList_SET_ITEM(chunks, i, chunk_dict);
    }
    
    PyDict_SetItemString(result, "chunks", chunks);
    Py_DECREF(chunks);
    
    return result;
}