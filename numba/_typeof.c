#include <string.h>
#include <time.h>
#include <assert.h>

#include "_pymodule.h"
#include "_typeof.h"
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/ndarrayobject.h>


/* Cached typecodes for basic scalar types */
static int tc_int8;
static int tc_int16;
static int tc_int32;
static int tc_int64;
static int tc_uint8;
static int tc_uint16;
static int tc_uint32;
static int tc_uint64;
static int tc_float32;
static int tc_float64;
static int tc_complex64;
static int tc_complex128;
static int BASIC_TYPECODES[12];

static int tc_intp;

static PyObject* typecache;
static PyObject* ndarray_typecache;

static PyObject *str_typeof_pyval = NULL;


/*
 * Type fingerprint computation.
 */

typedef struct {
    char *buf;
    size_t n;
    size_t allocated;
    char static_buf[256];
} string_writer_t;

static void
string_writer_init(string_writer_t *w)
{
    w->buf = w->static_buf;
    w->n = 0;
    w->allocated = sizeof(w->static_buf) / sizeof(unsigned char);
}

static void
string_writer_clear(string_writer_t *w)
{
    if (w->buf != w->static_buf)
        free(w->buf);
}

static inline int
string_writer_ensure(string_writer_t *w, size_t bytes)
{
    size_t newsize;
    if (w->n + bytes <= w->allocated)
        return 0;
    newsize = (w->allocated << 2) + 1;
    if (w->buf == w->static_buf)
        w->buf = malloc(newsize);
    else
        w->buf = realloc(w->buf, newsize);
    if (w->buf) {
        return 0;
    }
    else {
        PyErr_NoMemory();
        return -1;
    }
}

static inline int
string_writer_put_char(string_writer_t *w, unsigned char c)
{
    if (string_writer_ensure(w, 1))
        return -1;
    w->buf[w->n++] = c;
    return 0;
}

static inline int
string_writer_put_int32(string_writer_t *w, unsigned int v)
{
    if (string_writer_ensure(w, 4))
        return -1;
    w->buf[w->n] = v & 0xff;
    w->buf[w->n + 1] = (v >> 8) & 0xff;
    w->buf[w->n + 2] = (v >> 16) & 0xff;
    w->buf[w->n + 3] = (v >> 24) & 0xff;
    w->n += 4;
    return 0;
}

static inline int
string_writer_put_intp(string_writer_t *w, npy_intp v)
{
    const int N = sizeof(npy_intp);
    if (string_writer_ensure(w, N))
        return -1;
    w->buf[w->n] = v & 0xff;
    w->buf[w->n + 1] = (v >> 8) & 0xff;
    w->buf[w->n + 2] = (v >> 16) & 0xff;
    w->buf[w->n + 3] = (v >> 24) & 0xff;
    if (N > 4) {
        w->buf[w->n + 4] = (v >> 32) & 0xff;
        w->buf[w->n + 5] = (v >> 40) & 0xff;
        w->buf[w->n + 6] = (v >> 48) & 0xff;
        w->buf[w->n + 7] = (v >> 56) & 0xff;
    }
    w->n += N;
    return 0;
}

enum opcode {
    OP_START_TUPLE = '(',
    OP_END_TUPLE = ')',
    OP_INT = 'i',
    OP_FLOAT = 'f',
    OP_COMPLEX = 'c',
    OP_BOOL = '?',
    OP_BYTEARRAY = 'a',
    OP_BYTES = 'b',
    OP_NONE = 'n',

    OP_NP_SCALAR = 'S',
    OP_NP_ARRAY = 'A',
    OP_NP_DTYPE = 'D'
};

#define TRY(func, w, arg) \
    do { \
        if (func(w, arg)) return -1; \
    } while (0)


static int
fingerprint_unrecognized(PyObject *val)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "cannot compute type fingerprint for value");
    return -1;
}

static int
compute_dtype_fingerprint(string_writer_t *w, PyArray_Descr *descr)
{
    int typenum = descr->type_num;
    if (typenum < NPY_OBJECT)
        return string_writer_put_char(w, (char) typenum);
    if (typenum == NPY_VOID) {
        /* Structured dtype, serialize the dtype pointer.  In most cases,
         * people reuse the same dtype instance; otherwise the cache
         * will just be less efficient (but as correct).
         */
        TRY(string_writer_put_char, w, (char) typenum);
        return string_writer_put_intp(w, (npy_intp) descr);
    }
    if (PyTypeNum_ISDATETIME(typenum)) {
        PyArray_DatetimeMetaData *md;
        md = &(((PyArray_DatetimeDTypeMetaData *)descr->c_metadata)->meta);
        TRY(string_writer_put_char, w, (char) typenum);
        TRY(string_writer_put_char, w, (char) md->base);
        return string_writer_put_int32(w, (char) md->num);
    }

    return fingerprint_unrecognized((PyObject *) descr);
}

static int
compute_fingerprint(string_writer_t *w, PyObject *val)
{
    if (val == Py_None)
        return string_writer_put_char(w, OP_NONE);
    if (PyBool_Check(val))
        return string_writer_put_char(w, OP_BOOL);
    if (PyInt_Check(val) || PyLong_Check(val))
        return string_writer_put_char(w, OP_INT);
    if (PyFloat_Check(val))
        return string_writer_put_char(w, OP_FLOAT);
    if (PyComplex_CheckExact(val))
        return string_writer_put_char(w, OP_COMPLEX);
    if (PyTuple_Check(val)) {
        Py_ssize_t i, n;
        n = PyTuple_GET_SIZE(val);
        TRY(string_writer_put_char, w, OP_START_TUPLE);
        for (i = 0; i < n; i++)
            TRY(compute_fingerprint, w, PyTuple_GET_ITEM(val, i));
        TRY(string_writer_put_char, w, OP_END_TUPLE);
        return 0;
    }
    if (PyBytes_Check(val))
        return string_writer_put_char(w, OP_BYTES);
    if (PyByteArray_Check(val))
        return string_writer_put_char(w, OP_BYTEARRAY);
    if (PyArray_IsScalar(val, Generic)) {
        /* Note: PyArray_DescrFromScalar() may be a bit slow on
           non-trivial types. */
        PyArray_Descr *descr = PyArray_DescrFromScalar(val);
        if (descr == NULL)
            return -1;
        TRY(string_writer_put_char, w, OP_NP_SCALAR);
        TRY(compute_dtype_fingerprint, w, descr);
        Py_DECREF(descr);
        return 0;
    }
    if (PyArray_Check(val)) {
        PyArrayObject *ary = (PyArrayObject *) val;
        int ndim = PyArray_NDIM(ary);

        TRY(string_writer_put_char, w, OP_NP_ARRAY);
        TRY(string_writer_put_int32, w, ndim);
        if (PyArray_IS_C_CONTIGUOUS(ary))
            TRY(string_writer_put_char, w, 'C');
        else if (PyArray_IS_F_CONTIGUOUS(ary))
            TRY(string_writer_put_char, w, 'F');
        else
            TRY(string_writer_put_char, w, 'A');
        if (PyArray_ISWRITEABLE(ary))
            TRY(string_writer_put_char, w, 'W');
        else
            TRY(string_writer_put_char, w, 'R');
        return compute_dtype_fingerprint(w, PyArray_DESCR(ary));
    }
    if (PyArray_DescrCheck(val)) {
        TRY(string_writer_put_char, w, OP_NP_DTYPE);
        return compute_dtype_fingerprint(w, (PyArray_Descr *) val);
    }

    /* Type not recognized */
    return fingerprint_unrecognized(val);
}

PyObject *
typeof_compute_fingerprint(PyObject *val)
{
    PyObject *res;
    string_writer_t w;

    string_writer_init(&w);

    if (compute_fingerprint(&w, val))
        goto error;
    res = PyBytes_FromStringAndSize(w.buf, w.n);

    string_writer_clear(&w);
    return res;

error:
    string_writer_clear(&w);
    return NULL;
}


/* When we want to cache the type's typecode for later lookup, we need to
   keep a reference to the returned type object so that it cannot be
   deleted. This is because of the following events occurring when first
   using a @jit function for a given set of types:

    1. typecode_fallback requests a new typecode for an arbitrary Python value;
       this implies creating a Numba type object (on the first dispatcher call);
       the typecode cache is then populated.
    2. matching of the typecode list in _dispatcherimpl.cpp fails, since the
       typecode is new.
    3. we have to compile: compile_and_invoke() is called, it will invoke
       Dispatcher_Insert to register the new signature.

   The reference to the Numba type object returned in step 1 is deleted as
   soon as we call Py_DECREF() on it, since we are holding the only
   reference. If this happens and we use the typecode we got to populate the
   cache, then the cache won't ever return the correct typecode, and the
   dispatcher will never successfully match the typecodes with those of
   some already-compiled instance. So we need to make sure that we don't
   call Py_DECREF() on objects whose typecode will be used to populate the
   cache. This is ensured by calling _typecode_fallback with
   retain_reference == 0.

   Note that technically we are leaking the reference, since we do not continue
   to hold a pointer to the type object that we get back from typeof_pyval.
   However, we don't need to refer to it again, we just need to make sure that
   it is never deleted.
*/
static
int _typecode_fallback(PyObject *dispatcher, PyObject *val,
                       int retain_reference) {
    PyObject *tmptype, *tmpcode;
    int typecode;

    // Go back to the interpreter
    tmptype = PyObject_CallMethodObjArgs((PyObject *) dispatcher,
                                         str_typeof_pyval, val, NULL);
    if (!tmptype) {
        return -1;
    }

    tmpcode = PyObject_GetAttrString(tmptype, "_code");
    if (!retain_reference) {
        Py_DECREF(tmptype);
    }
    if (tmpcode == NULL) {
        return -1;
    }
    typecode = PyLong_AsLong(tmpcode);
    Py_DECREF(tmpcode);
    return typecode;
}

/* Variations on _typecode_fallback for convenience */

static
int typecode_fallback(PyObject *dispatcher, PyObject *val) {
    return _typecode_fallback(dispatcher, val, 0);
}

static
int typecode_fallback_keep_ref(PyObject *dispatcher, PyObject *val) {
    return _typecode_fallback(dispatcher, val, 1);
}

/*
 * Direct lookup table for extra-fast typecode resolution of simple array types.
 */

#define N_DTYPES 12
#define N_NDIM 5    /* Fast path for up to 5D array */
#define N_LAYOUT 3
static int cached_arycode[N_NDIM][N_LAYOUT][N_DTYPES];

/* Convert a Numpy dtype number to an internal index into cached_arycode.
   The returned value must also be a valid index into BASIC_TYPECODES. */
static int dtype_num_to_typecode(int type_num) {
    int dtype;
    switch(type_num) {
    case NPY_INT8:
        dtype = 0;
        break;
    case NPY_INT16:
        dtype = 1;
        break;
    case NPY_INT32:
        dtype = 2;
        break;
    case NPY_INT64:
        dtype = 3;
        break;
    case NPY_UINT8:
        dtype = 4;
        break;
    case NPY_UINT16:
        dtype = 5;
        break;
    case NPY_UINT32:
        dtype = 6;
        break;
    case NPY_UINT64:
        dtype = 7;
        break;
    case NPY_FLOAT32:
        dtype = 8;
        break;
    case NPY_FLOAT64:
        dtype = 9;
        break;
    case NPY_COMPLEX64:
        dtype = 10;
        break;
    case NPY_COMPLEX128:
        dtype = 11;
        break;
    default:
        /* Type not included in the global lookup table */
        dtype = -1;
    }
    return dtype;
}

static
int get_cached_typecode(PyArray_Descr* descr) {
    PyObject* tmpobject = PyDict_GetItem(typecache, (PyObject*)descr);
    if (tmpobject == NULL)
        return -1;

    return PyLong_AsLong(tmpobject);
}

static
void cache_typecode(PyArray_Descr* descr, int typecode) {
    PyObject* value = PyLong_FromLong(typecode);
    PyDict_SetItem(typecache, (PyObject*)descr, value);
    Py_DECREF(value);
}

static
PyObject* ndarray_key(int ndim, int layout, PyArray_Descr* descr) {
    PyObject* tmpndim = PyLong_FromLong(ndim);
    PyObject* tmplayout = PyLong_FromLong(layout);
    PyObject* key = PyTuple_Pack(3, tmpndim, tmplayout, descr);
    Py_DECREF(tmpndim);
    Py_DECREF(tmplayout);
    return key;
}

static
int get_cached_ndarray_typecode(int ndim, int layout, PyArray_Descr* descr) {
    PyObject* key = ndarray_key(ndim, layout, descr);
    PyObject *tmpobject = PyDict_GetItem(ndarray_typecache, key);
    if (tmpobject == NULL)
        return -1;

    Py_DECREF(key);
    return PyLong_AsLong(tmpobject);
}

static
void cache_ndarray_typecode(int ndim, int layout, PyArray_Descr* descr,
                            int typecode) {
    PyObject* key = ndarray_key(ndim, layout, descr);
    PyObject* value = PyLong_FromLong(typecode);
    PyDict_SetItem(ndarray_typecache, key, value);
    Py_DECREF(key);
    Py_DECREF(value);
}

static
int typecode_ndarray(PyObject *dispatcher, PyArrayObject *ary) {
    int typecode;
    int dtype;
    int ndim = PyArray_NDIM(ary);
    int layout = 0;

    /* The order in which we check for the right contiguous-ness is important.
       The order must match the order by numba.numpy_support.map_layout.
    */
    if (PyArray_ISCARRAY(ary)){
        layout = 1;
    } else if (PyArray_ISFARRAY(ary)) {
        layout = 2;
    }

    if (ndim <= 0 || ndim > N_NDIM) goto FALLBACK;

    dtype = dtype_num_to_typecode(PyArray_TYPE(ary));
    if (dtype == -1) goto FALLBACK;

    /* Fast path, using direct table lookup */
    assert(layout < N_LAYOUT);
    assert(ndim <= N_NDIM);
    assert(dtype < N_DTYPES);

    typecode = cached_arycode[ndim - 1][layout][dtype];
    if (typecode == -1) {
        /* First use of this table entry, so it requires populating */
        typecode = typecode_fallback_keep_ref(dispatcher, (PyObject*)ary);
        cached_arycode[ndim - 1][layout][dtype] = typecode;
    }
    return typecode;

FALLBACK:
    /* Slower path, for non-trivial array types */

    /* If this isn't a structured array then we can't use the cache */
    if (PyArray_TYPE(ary) != NPY_VOID)
        return typecode_fallback(dispatcher, (PyObject*)ary);

    /* Check type cache */
    typecode = get_cached_ndarray_typecode(ndim, layout, PyArray_DESCR(ary));
    if (typecode == -1) {
        /* First use of this type, use fallback and populate the cache */
        typecode = typecode_fallback_keep_ref(dispatcher, (PyObject*)ary);
        cache_ndarray_typecode(ndim, layout, PyArray_DESCR(ary), typecode);
    }
    return typecode;
}

static
int typecode_arrayscalar(PyObject *dispatcher, PyObject* aryscalar) {
    int typecode;
    PyArray_Descr* descr;
    descr = PyArray_DescrFromScalar(aryscalar);
    if (!descr)
        return typecode_fallback(dispatcher, aryscalar);

    if (descr->type_num == NPY_VOID) {
        typecode = get_cached_typecode(descr);
        if (typecode == -1) {
            /* Resolve through fallback then populate cache */
            typecode = typecode_fallback_keep_ref(dispatcher, aryscalar);
            cache_typecode(descr, typecode);
        }
        Py_DECREF(descr);
        return typecode;
    }

    typecode = dtype_num_to_typecode(descr->type_num);
    Py_DECREF(descr);
    if (typecode == -1)
        return typecode_fallback(dispatcher, aryscalar);
    return BASIC_TYPECODES[typecode];
}

int
typeof_typecode(PyObject *dispatcher, PyObject *val)
{
    PyTypeObject *tyobj = val->ob_type;
    /* This needs to be kept in sync with Dispatcher.typeof_pyval(),
     * otherwise funny things may happen.
     */
    if (tyobj == &PyInt_Type || tyobj == &PyLong_Type)
        return tc_intp;
    else if (tyobj == &PyFloat_Type)
        return tc_float64;
    else if (tyobj == &PyComplex_Type)
        return tc_complex128;
    /* Array scalar handling */
    else if (PyArray_CheckScalar(val)) {
        return typecode_arrayscalar(dispatcher, val);
    }
    /* Array handling */
    else if (PyType_IsSubtype(tyobj, &PyArray_Type)) {
        return typecode_ndarray(dispatcher, (PyArrayObject*)val);
    }

    return typecode_fallback(dispatcher, val);
}

PyObject *
typeof_init(PyObject *self, PyObject *args)
{
    PyObject *tmpobj;
    PyObject* dict = PySequence_Fast_GET_ITEM(args, 0);
    int index = 0;

    /* Initialize Numpy API */
    import_array();

    #define UNWRAP_TYPE(S)                                              \
        if(!(tmpobj = PyDict_GetItemString(dict, #S))) return NULL;     \
        else {  tc_##S = PyLong_AsLong(tmpobj);                         \
                BASIC_TYPECODES[index++] = tc_##S;  }

    UNWRAP_TYPE(int8)
    UNWRAP_TYPE(int16)
    UNWRAP_TYPE(int32)
    UNWRAP_TYPE(int64)

    UNWRAP_TYPE(uint8)
    UNWRAP_TYPE(uint16)
    UNWRAP_TYPE(uint32)
    UNWRAP_TYPE(uint64)

    UNWRAP_TYPE(float32)
    UNWRAP_TYPE(float64)

    UNWRAP_TYPE(complex64)
    UNWRAP_TYPE(complex128)

    #undef UNWRAP_TYPE

    switch(sizeof(void*)) {
    case 4:
        tc_intp = tc_int32;
        break;
    case 8:
        tc_intp = tc_int64;
        break;
    default:
        PyErr_SetString(PyExc_AssertionError, "sizeof(void*) != {4, 8}");
        return NULL;
    }

    typecache = PyDict_New();
    ndarray_typecache = PyDict_New();
    if (typecache == NULL || ndarray_typecache == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create type cache");
        return NULL;
    }

    /* initialize cached_arycode to all ones (in bits) */
    memset(cached_arycode, 0xFF, sizeof(cached_arycode));

    str_typeof_pyval = PyString_InternFromString("typeof_pyval");
    if (str_typeof_pyval == NULL)
        return NULL;

    Py_RETURN_NONE;
}
