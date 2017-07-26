#include <Python.h>

#include "xcoin.h"

static PyObject *xcoin_getpowhash(PyObject *self, PyObject *args)
{
    char *output;
    PyObject *value;
#if PY_MAJOR_VERSION >= 3
    PyBytesObject *input;
#else
    PyStringObject *input;
#endif
    if (!PyArg_ParseTuple(args, "S", &input))
        return NULL;
    Py_INCREF(input);
    output = PyMem_Malloc(32);

#if PY_MAJOR_VERSION >= 3
    xcoin_hash((char *)PyBytes_AsString((PyObject*) input), output);
#else
    xcoin_hash((char *)PyString_AsString((PyObject*) input), output);
#endif
    Py_DECREF(input);
#if PY_MAJOR_VERSION >= 3
    value = Py_BuildValue("y#", output, 32);
#else
    value = Py_BuildValue("s#", output, 32);
#endif
    PyMem_Free(output);
    return value;
}

static PyMethodDef XcoinMethods[] = {
    { "getPoWHash", xcoin_getpowhash, METH_VARARGS, "Returns the proof of work hash using xcoin hash" },
    { NULL, NULL, 0, NULL }
};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef XcoinModule = {
    PyModuleDef_HEAD_INIT,
    "xcoin_hash",
    "...",
    -1,
    XcoinMethods
};

PyMODINIT_FUNC PyInit_xcoin_hash(void) {
    return PyModule_Create(&XcoinModule);
}

#else

PyMODINIT_FUNC initxcoin_hash(void) {
    (void) Py_InitModule("xcoin_hash", XcoinMethods);
}
#endif
