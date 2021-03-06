#include <Python.h>
#ifdef __linux__
    #include <arpa/inet.h>
#elif _WIN32
    #include <Winsock2.h>
#endif
#include "../libblossom/bloom.h"
#include "crc32.c"

static char module_docstring[] = "Python wrapper for libbloom";

typedef struct {
    PyObject_HEAD
    struct bloom *_bloom_struct;
} Filter;

static PyTypeObject FilterType = {
    PyObject_HEAD_INIT(NULL)
    0,                          /*ob_size*/
    "pyblossom.Filter",         /*tp_name*/
    sizeof(Filter),             /*tp_basicsize*/
    0,                          /*tp_itemsize*/
    0,                          /*tp_dealloc*/
    0,                          /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,                          /*tp_compare*/
    0,                          /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    0,                          /*tp_hash */
    0,                          /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,         /*tp_flags*/
    "Filter objects",           /*tp_doc*/
};

struct serialized_filter_header {
    uint16_t checksum;
    uint16_t error_rate;
    uint32_t cardinality;
};

static PyObject *PyBlossomError;

static PyObject *
instantiate_filter(uint32_t cardinality, uint16_t error_rate, const char *data, int datalen)
{
    PyObject *args = Py_BuildValue("(ids#)", cardinality, 1.0 / error_rate, data, datalen);
    PyObject *obj = FilterType.tp_new(&FilterType, args, NULL);
    if (FilterType.tp_init(obj, args, NULL) < 0) {
        Py_DECREF(obj);
        obj = NULL;
    }
    return obj;
}

/* helpers */
static uint16_t
compute_checksum(const char *buf, size_t len)
{
    uint32_t checksum32 = crc32(0, buf, len);
    return (checksum32 & 0xFFFF) ^ (checksum32 >> 16);
}

static uint16_t
read_uint16(const char **buffer)
{
    uint16_t ret = ntohs(*((uint16_t *)*buffer));
    *buffer += sizeof(uint16_t);
    return ret;
}

static uint32_t
read_uint32(const char **buffer)
{
    uint32_t ret = ntohl(*((uint32_t *)*buffer));
    *buffer += sizeof(uint32_t);
    return ret;
}


/* serialization */
static PyObject *
load(PyObject *self, PyObject *args)
{
    struct serialized_filter_header header;
    const char *data;
    size_t datalen;
    uint16_t expected_checksum;

    const char *buffer;
    Py_ssize_t buflen;
    if (!PyArg_ParseTuple(args, "s#", &buffer, &buflen)) {
        return NULL;
    }

    if (buflen < sizeof(struct serialized_filter_header) + 1) {
        PyErr_SetString(PyBlossomError, "incomplete payload");
        return NULL;
    }

    header.checksum = read_uint16(&buffer);
    header.error_rate = read_uint16(&buffer);
    header.cardinality = read_uint32(&buffer);
    data = buffer;
    datalen = buflen - sizeof(struct serialized_filter_header);
    expected_checksum = compute_checksum(data, datalen);
    if (expected_checksum != header.checksum) {
        PyErr_SetString(PyBlossomError, "checksum mismatch");
        return NULL;
    }
    return instantiate_filter(header.cardinality, header.error_rate, data, datalen);
}

static PyObject *
dump(PyObject *self, PyObject *args)
{
    uint16_t checksum;
    struct serialized_filter_header header;
    PyObject *serial_header;
    PyObject *serial_data;

    Filter *filter;
    if (!PyArg_ParseTuple(args, "O", &filter)) {
        return NULL;
    }

    checksum = compute_checksum((const char *)filter->_bloom_struct->bf, filter->_bloom_struct->bytes);
    header.checksum = htons(checksum);
    header.error_rate = htons(1.0 / filter->_bloom_struct->error);
    header.cardinality = htonl(filter->_bloom_struct->entries);

    serial_header = PyString_FromStringAndSize((const char *)&header, sizeof(struct serialized_filter_header));
    serial_data = PyString_FromStringAndSize((const char *)filter->_bloom_struct->bf, filter->_bloom_struct->bytes);
    PyString_Concat(&serial_header, serial_data);
    return serial_header;
}

static PyObject *
dump_ex(PyObject *self, PyObject *args)
{
    Filter *filter;
    struct bloom *bloom_struct;
    Py_buffer pybuf;
    PyObject *memview;

    if (!PyArg_ParseTuple(args, "O", &filter)) {
        return NULL;
    }

    bloom_struct = filter->_bloom_struct;
    PyBuffer_FillInfo(&pybuf, NULL, bloom_struct->bf, bloom_struct->bytes, 1, 
        PyBUF_CONTIG_RO);
    memview = PyMemoryView_FromBuffer(&pybuf);
    return Py_BuildValue("idO", bloom_struct->entries, bloom_struct->error, 
        memview);
}

static PyMethodDef module_methods[] = {
    {"load", (PyCFunction)load, METH_VARARGS,
     "load a serialized filter"},
    {"dump", (PyCFunction)dump, METH_VARARGS,
     "dump a filter into a string"},
    {"dump_ex", (PyCFunction)dump_ex, METH_VARARGS,
     "dump a filter and return it as memory view with params (without crc32 check)"},
    {NULL}
};

/* Filter methods */
static PyObject *
Filter_add(Filter *self, PyObject *args)
{
    const char *buffer;
    Py_ssize_t buflen;
    if (!PyArg_ParseTuple(args, "s#", &buffer, &buflen)) {
        return NULL;
    }

    bloom_add(self->_bloom_struct, buffer, buflen);
    Py_RETURN_NONE;
}

static PyObject *
Filter_check(Filter *self, PyObject *args)
{
    const char *buffer;
    Py_ssize_t buflen;
    if (!PyArg_ParseTuple(args, "s#", &buffer, &buflen)) {
        return NULL;
    }

    if (bloom_check(self->_bloom_struct, buffer, buflen))
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *
Filter_get_buffer(Filter *self, PyObject *args)
{
    Py_buffer pybuf;
    PyObject *memview;
    struct bloom *bloom_struct;

    bloom_struct = self->_bloom_struct;
    PyBuffer_FillInfo(&pybuf, NULL, bloom_struct->bf, bloom_struct->bytes, 
        0, PyBUF_CONTIG);
    memview = PyMemoryView_FromBuffer(&pybuf);
    return Py_BuildValue("O", memview);
}

static PyMethodDef Filter_methods[] = {
    {"add", (PyCFunction)Filter_add, METH_VARARGS,
     "add a member to the filter"},
    {"contains", (PyCFunction)Filter_check, METH_VARARGS,
     "check if member exists the filter"},
    {"get_buffer", (PyCFunction)Filter_get_buffer, METH_NOARGS,
     "get writable memoryview of the internal buffer"},
    {NULL}  /* Sentinel */
};

static void
Filter_dealloc(Filter* self)
{
    bloom_free(self->_bloom_struct);
    free(self->_bloom_struct);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
Filter_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Filter *self;

    self = (Filter *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->_bloom_struct = (struct bloom *)malloc(sizeof(struct bloom));
        if (self->_bloom_struct == NULL)
            return PyErr_NoMemory();
    }

    return (PyObject *)self;
}

static int
Filter_init(Filter *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"entries", "error", "data", NULL};
    int entries, success;
    double error;
    PyObject *buf_src = NULL;
    Py_buffer buf;
    struct bloom *bloom_struct;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "id|O", kwlist, &entries, &error, &buf_src)) {
        return -1;
    }

    bloom_struct = self->_bloom_struct;
    success = bloom_init(bloom_struct, entries, error);

    if (success == 0) {
        if (buf_src != NULL) {
            if (!PyObject_CheckBuffer(buf_src)) {
                PyErr_SetString(PyBlossomError, "buffer interface is not supported by provided data type");
                return -1;
            }

            if (PyObject_GetBuffer(buf_src, &buf, PyBUF_CONTIG_RO) < 0) {
                PyErr_SetString(PyBlossomError, "could not get buffer from provided data type");
                return -1;
            }

            if ((int)buf.len != bloom_struct->bytes) {
                PyErr_SetString(PyBlossomError, "invalid data length");
                return -1;
            }
            memcpy(bloom_struct->bf, (const unsigned char *)buf.buf, 
                bloom_struct->bytes);
        }
        return 0;
    }
    else {
        PyErr_SetString(PyBlossomError, "internal initialization failed");
        return -1;
    }
}


#ifndef PyMODINIT_FUNC
#define PyMODINIT_FUND void
#endif
PyMODINIT_FUNC
initpyblossom(void)
{
    PyObject *m;
    FilterType.tp_new = Filter_new;
    FilterType.tp_init = (initproc)Filter_init;
    FilterType.tp_methods = Filter_methods;
    FilterType.tp_dealloc = (destructor)Filter_dealloc;
    if (PyType_Ready(&FilterType) < 0)
        return;

    m = Py_InitModule3("pyblossom", module_methods, module_docstring);
    Py_INCREF(&FilterType);
    PyModule_AddObject(m, "Filter", (PyObject *)&FilterType);

    PyBlossomError = PyErr_NewException("pyblossom.error", NULL, NULL);
    Py_INCREF(PyBlossomError);
    PyModule_AddObject(m, "error", PyBlossomError);
}
