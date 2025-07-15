#include "Python.h"
#include "pycore_coro_metrics.h"
#include "pycore_genobject.h"
#include "pycore_time.h"
#include "pycore_frame.h"
#include "pycore_code.h"
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
            if (metrics) {
                if (metrics->chunks) {
                    /* Free any owned references */
                    for (int i = 0; i < metrics->chunk_count; i++) {
                        Py_XDECREF(metrics->chunks[i].awaited_name);
                        Py_XDECREF(metrics->chunks[i].filename);
                    }
                    PyMem_Free(metrics->chunks);
                }
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
        
        metrics->chunk_capacity = CORO_MAX_CHUNKS; /* Use max capacity */
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
_PyCoroMetrics_EndChunk(PyObject *coro, _PyInterpreterFrame *frame)
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
    
    /* Only keep the longest chunks - find insertion point */
    int insert_pos = -1;
    
    if (metrics->chunk_count < CORO_MAX_CHUNKS) {
        /* Still have room, add at the end */
        insert_pos = metrics->chunk_count;
    } else {
        /* Find if this chunk is longer than any existing chunk */
        for (int i = metrics->chunk_count - 1; i >= 0; i--) {
            if (duration > metrics->chunks[i].duration) {
                insert_pos = i;
            } else {
                break;
            }
        }
        
        if (insert_pos == -1) {
            /* This chunk is shorter than all existing chunks */
            metrics->is_tracking = 0;
            return;
        }
    }
    
    /* Try to get the awaited function name from the frame */
    PyObject *awaited_name = NULL;
    PyObject *filename = NULL;
    int lineno = 0;
    
    if (frame != NULL) {
        PyCodeObject *code = _PyFrame_GetCode(frame);
        if (code != NULL) {
            /* Get the line number */
            lineno = PyCode_Addr2Line(code, _PyInterpreterFrame_LASTI(frame));
            
            /* Get the filename */
            if (code->co_filename != NULL) {
                filename = code->co_filename;
                Py_INCREF(filename);
            }
            
            /* Try to get the name of what we're awaiting from the stack */
            if (frame->stacktop > code->co_nlocalsplus) {
                PyObject *top = frame->localsplus[frame->stacktop - 1];
                if (top != NULL) {
                    /* Check if it's a coroutine */
                    if (PyCoro_CheckExact(top)) {
                        PyObject *coro_name = ((PyCoroObject *)top)->cr_name;
                        if (coro_name != NULL) {
                            awaited_name = coro_name;
                            Py_INCREF(awaited_name);
                        }
                    }
                    /* Check if it's an async generator */
                    else if (PyAsyncGen_CheckExact(top)) {
                        PyObject *gen_name = ((PyAsyncGenObject *)top)->ag_name;
                        if (gen_name != NULL) {
                            awaited_name = gen_name;
                            Py_INCREF(awaited_name);
                        }
                    }
                    /* Check if it's a Task or Future */
                    else if (PyObject_HasAttrString(top, "__name__")) {
                        awaited_name = PyObject_GetAttrString(top, "__name__");
                        if (awaited_name == NULL) {
                            PyErr_Clear();
                        }
                    }
                }
            }
        }
    }
    
    /* Create new chunk data */
    CoroChunkMetric new_chunk = {
        .start_time = metrics->current_chunk_start,
        .duration = duration,
        .awaited_name = awaited_name,
        .filename = filename,
        .lineno = lineno
    };
    
    /* Insert the chunk in sorted order (by duration, descending) */
    if (metrics->chunk_count < CORO_MAX_CHUNKS) {
        /* Make room at insert_pos */
        for (int i = metrics->chunk_count; i > insert_pos; i--) {
            metrics->chunks[i] = metrics->chunks[i-1];
        }
        metrics->chunks[insert_pos] = new_chunk;
        metrics->chunk_count++;
    } else {
        /* We're at capacity - need to remove shortest chunk */
        /* First, free the reference of the chunk being removed */
        Py_XDECREF(metrics->chunks[CORO_MAX_CHUNKS - 1].awaited_name);
        Py_XDECREF(metrics->chunks[CORO_MAX_CHUNKS - 1].filename);
        
        /* Shift chunks to make room */
        for (int i = CORO_MAX_CHUNKS - 1; i > insert_pos; i--) {
            metrics->chunks[i] = metrics->chunks[i-1];
        }
        metrics->chunks[insert_pos] = new_chunk;
    }
    
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
                /* Free any owned references */
                for (int i = 0; i < metrics->chunk_count; i++) {
                    Py_XDECREF(metrics->chunks[i].awaited_name);
                    Py_XDECREF(metrics->chunks[i].filename);
                }
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
        
        /* Add awaited function name if available */
        if (metrics->chunks[i].awaited_name != NULL) {
            PyDict_SetItemString(chunk_dict, "awaited", metrics->chunks[i].awaited_name);
        } else {
            PyDict_SetItemString(chunk_dict, "awaited", Py_None);
        }
        
        /* Add filename if available */
        if (metrics->chunks[i].filename != NULL) {
            PyDict_SetItemString(chunk_dict, "filename", metrics->chunks[i].filename);
        } else {
            PyDict_SetItemString(chunk_dict, "filename", Py_None);
        }
        
        /* Add line number */
        PyObject *lineno = PyLong_FromLong(metrics->chunks[i].lineno);
        if (lineno == NULL) {
            Py_DECREF(chunk_dict);
            Py_DECREF(chunks);
            Py_DECREF(result);
            return NULL;
        }
        PyDict_SetItemString(chunk_dict, "lineno", lineno);
        Py_DECREF(lineno);
        
        PyList_SET_ITEM(chunks, i, chunk_dict);
    }
    
    PyDict_SetItemString(result, "chunks", chunks);
    Py_DECREF(chunks);
    
    return result;
}