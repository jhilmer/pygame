/*
  pygame - Python Game Library
  Copyright (C) 2007  Marcus von Appen

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#define PYGAMEAPI_PIXELARRAY_INTERNAL
        
#include "pygame.h"
#include "pygamedocs.h"
#include "surface.h"

#if PY_VERSION_HEX < 0x02050000
#define PyIndex_Check(op) 0
#endif

#define GET_SLICE_VALS(array, start, end, ylen, ystep, xlen, xstep, padding, \
    _low, _high, _step, _pad)                                           \
    start = array->start + _low * array->xstep;                         \
    end = (_high - _low > 0) ?                                          \
        array->end - array->xlen + _high * array->xstep :               \
        array->end - array->xlen + _low * array->xstep;                 \
    ylen = array->ylen;                                                 \
    ystep = array->ystep;                                               \
    xlen = (_high - _low < 0) ? _low - _high : _high - _low;            \
    xstep = array->xstep * _step;                                       \
    padding = _pad;


/*
  end = array->end - array->xlen + _high * array->xstep;
  xstep = (_step > 1) ? (array->xstep + _step) : array->xstep;
*/

typedef struct
{
    PyObject_HEAD
    PyObject *dict;     /* dict for subclassing */
    PyObject *weakrefs; /* Weakrefs for subclassing */
    PyObject *surface;  /* Surface associated with the array. */
    PyObject *lock;     /* Lock object for the surface. */
    Uint32 start;       /* Start offset for subarrays */
    Uint32 end;         /* End offset for subarrays */
    Uint32 xlen;        /* X segment length. */
    Uint32 ylen;        /* Y segment length. */
    Sint32 xstep;       /* X offset step width. */
    Sint32 ystep;       /* Y offset step width. */
    Uint32 padding;     /* Padding to get to the next x offset. */
    PyObject *parent;   /* Parent pixel array */

} PyPixelArray;

static PyPixelArray* _pxarray_new_internal (PyTypeObject *type,
    PyObject *surface, Uint32 start, Uint32 end, Uint32 xlen, Uint32 ylen,
    Sint32 xstep, Sint32 ystep, Uint32 padding, PyObject *parent);
static PyObject* _pxarray_new (PyTypeObject *type, PyObject *args,
    PyObject *kwds);
static void _pxarray_dealloc (PyPixelArray *self);

static PyObject* _pxarray_get_dict (PyPixelArray *self, void *closure);
static PyObject* _pxarray_get_surface (PyPixelArray *self, void *closure);

static PyObject* _pxarray_repr (PyPixelArray *array);

static int _get_color_from_object (PyObject *val, SDL_PixelFormat *format,
    Uint32 *color);
static PyObject* _get_single_pixel (Uint8 *pixels, int bpp, Uint32 _index,
    Uint32 row);
static void _set_single_pixel (Uint8 *pixels, int bpp, Uint32 _index,
    Uint32 row, SDL_PixelFormat *format, Uint32 color);
static PyObject* _array_slice_internal (PyPixelArray *array, Sint32 _start,
    Sint32 _end, Sint32 _step);

/* Sequence methods */
static Py_ssize_t _pxarray_length (PyPixelArray *array);
static PyObject* _pxarray_concat (PyPixelArray *array, PyObject *value);
static PyObject* _pxarray_repeat (PyPixelArray *a, Py_ssize_t n);
static PyObject* _pxarray_item (PyPixelArray *array, Py_ssize_t _index);
static PyObject* _pxarray_slice (PyPixelArray *array, Py_ssize_t low,
    Py_ssize_t high);
static int _array_assign_array (PyPixelArray *array, Py_ssize_t low,
    Py_ssize_t high, PyPixelArray *val);
static int _array_assign_sequence (PyPixelArray *array, Py_ssize_t low,
    Py_ssize_t high, PyObject *val);
static int _pxarray_ass_item (PyPixelArray *array, Py_ssize_t _index,
    PyObject *value);
static int _pxarray_ass_slice (PyPixelArray *array, Py_ssize_t low,
    Py_ssize_t high, PyObject *value);
static int _pxarray_contains (PyPixelArray *array, PyObject *value);
static PyObject* _pxarray_inplace_concat (PyPixelArray *array, PyObject *seq);
static PyObject* _pxarray_inplace_repeat (PyPixelArray *array, Py_ssize_t n);

/* Mapping methods */
static int _get_subslice (PyPixelArray *array, PyObject *op, Py_ssize_t length,
    Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step);
static PyObject* _pxarray_subscript (PyPixelArray *array, PyObject *op);
static int _pxarray_ass_subscript (PyPixelArray *array, PyObject* op,
    PyObject* value);

/* C API interfaces */
static PyObject* PyPixelArray_New (PyObject *surfobj);

/**
 * Methods, which are bound to the PyPixelArray type.
 */
static PyMethodDef _pxarray_methods[] =
{
    { NULL, NULL, 0, NULL }
};

/**
 * Getters and setters for the PyPixelArray.
 */
static PyGetSetDef _pxarray_getsets[] =
{
    { "__dict__", (getter) _pxarray_get_dict, NULL, NULL, NULL },
    { "surface", (getter) _pxarray_get_surface, NULL, DOC_PIXELARRAYSURFACE,
      NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

/**
 * Sequence interface support for the PyPixelArray.
 */
static PySequenceMethods _pxarray_sequence =
{
    (lenfunc) _pxarray_length,                  /*sq_length*/
    NULL, /*(binaryfunc) _pxarray_concat,*/     /*sq_concat*/
    (ssizeargfunc) _pxarray_repeat,             /*sq_repeat*/
    (ssizeargfunc) _pxarray_item,               /*sq_item*/
    (ssizessizeargfunc) _pxarray_slice,         /*sq_slice*/
    (ssizeobjargproc) _pxarray_ass_item,        /*sq_ass_item*/
    (ssizessizeobjargproc) _pxarray_ass_slice,  /*sq_ass_slice*/
    (objobjproc) _pxarray_contains,             /*sq_contains*/
    NULL, /*(binaryfunc) _pxarray_inplace_concat,*/       /*sq_inplace_concat*/
    NULL, /*(ssizeargfunc) _pxarray_inplace_repeat*/      /*sq_inplace_repeat*/
};

/**
 * Mapping interface support for the PyPixelArray.
 */
static PyMappingMethods _pxarray_mapping =
{
    (inquiry) _pxarray_length,              /*mp_length*/
    (binaryfunc) _pxarray_subscript,        /*mp_subscript*/
    (objobjargproc) _pxarray_ass_subscript, /*mp_ass_subscript*/
};

static PyTypeObject PyPixelArray_Type =
{
    PyObject_HEAD_INIT(NULL)
    0,
    "pygame.PixelArray",        /* tp_name */
    sizeof (PyPixelArray),      /* tp_basicsize */
    0,                          /* tp_itemsize */
    (destructor) _pxarray_dealloc, /* tp_dealloc */
    0,                          /* tp_print */
    0,                          /* tp_getattr */
    0,                          /* tp_setattr */
    0,                          /* tp_compare */
    (reprfunc) &_pxarray_repr,  /* tp_repr */
    0,                          /* tp_as_number */
    &_pxarray_sequence,         /* tp_as_sequence */
    0, /*&_pxarray_mapping,          /* tp_as_mapping */
    0,                          /* tp_hash */
    0,                          /* tp_call */
    0,                          /* tp_str */
    0,                          /* tp_getattro */
    0,                          /* tp_setattro */
    0,                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_WEAKREFS,
    DOC_PYGAMEPIXELARRAY,       /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    offsetof (PyPixelArray, weakrefs),  /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    _pxarray_methods,           /* tp_methods */
    0,                          /* tp_members */
    _pxarray_getsets,           /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    offsetof (PyPixelArray, dict), /* tp_dictoffset */
    0,                          /* tp_init */
    0,                          /* tp_alloc */
    _pxarray_new,               /* tp_new */
    0,                          /* tp_free */
    0,                          /* tp_is_gc */
    0,                          /* tp_bases */
    0,                          /* tp_mro */
    0,                          /* tp_cache */
    0,                          /* tp_subclasses */
    0,                          /* tp_weaklist */
    0                           /* tp_del */
};

#define PyPixelArray_Check(o) \
    ((o)->ob_type == (PyTypeObject *) &PyPixelArray_Type)

static PyPixelArray*
_pxarray_new_internal (PyTypeObject *type, PyObject *surface,
    Uint32 start, Uint32 end, Uint32 xlen, Uint32 ylen,
    Sint32 xstep, Sint32 ystep, Uint32 padding, PyObject *parent)
{
    PyPixelArray *self = (PyPixelArray *) type->tp_alloc (type, 0);
    if (!self)
        return NULL;

    self->surface = (PyObject *) surface;
    if (surface)
    {
        self->lock = PySurface_LockLifetime (surface);
        if (!self->lock)
        {
            self->ob_type->tp_free ((PyObject *) self);
            return NULL;
        }
    }
    self->weakrefs = NULL;
    self->dict = NULL;
    self->start = start;
    self->end = end;
    self->xlen = xlen;
    self->ylen = ylen;
    self->xstep = xstep;
    self->ystep = ystep;
    self->padding = padding;
    self->parent = parent;

    return self;
}

/**
 * Creates a new PyPixelArray.
 */
static PyObject*
_pxarray_new (PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *surfobj;
    SDL_Surface* surface;

    if (!PyArg_ParseTuple (args, "O!", &PySurface_Type, &surfobj))
        return NULL;

    surface = PySurface_AsSurface (surfobj);
    if (surface->format->BytesPerPixel < 1  ||
        surface->format->BytesPerPixel > 4)
        return RAISE (PyExc_ValueError,
            "unsupport bit depth for reference array");

    return (PyObject *) _pxarray_new_internal
        (type, surfobj, 0, (Uint32) surface->w * surface->h,
         (Uint32) surface->w, (Uint32) surface->h, 1, 1, surface->pitch,
         NULL);
}

/**
 * Deallocates the PyPixelArray and its members.
 */
static void
_pxarray_dealloc (PyPixelArray *self)
{
    if (self->weakrefs)
        PyObject_ClearWeakRefs ((PyObject *) self);
    if (self->lock)
    {
        Py_DECREF (self->lock);
    }
    Py_XDECREF (self->dict);
    self->ob_type->tp_free ((PyObject *) self);
}

/**** Getter and setter access ****/

/**
 * Getter for PixelArray.__dict__.
 */
static PyObject*
_pxarray_get_dict (PyPixelArray *self, void *closure)
{
    if (!self->dict)
    {
	self->dict = PyDict_New ();
        if (!self->dict)
            return NULL;
    }

    Py_INCREF (self->dict);
    return self->dict;
}

/**
 * Getter for PixelArray.surface
 */
static PyObject*
_pxarray_get_surface (PyPixelArray *self, void *closure)
{
    Py_XINCREF (self->surface);
    return self->surface;
}

/**** Methods ****/

/**
 * repr(PixelArray)
 */
static PyObject*
_pxarray_repr (PyPixelArray *array)
{
    PyObject *string;
    SDL_Surface *surface;
    int bpp;
    Uint8 *pixels;
    Uint8 *px24;
    Uint32 pixel;
    Uint32 x = 0;
    Uint32 y = 0;
    Sint32 xlen = 0;
    Sint32 absxstep;
    Sint32 absystep;
    Uint32 posx = 0;
    Uint32 posy = 0;

    surface = PySurface_AsSurface (array->surface);
    bpp = surface->format->BytesPerPixel;
    pixels = (Uint8 *) surface->pixels;
/*
    printf ("ARRAY: %d:%d, %d:%d %d:%d %d\n",
        array->start, array->end, array->xlen, array->xstep,
        array->ylen, array->ystep, array->padding);
*/
    string = PyString_FromString ("PixelArray(");

    absxstep = ABS (array->xstep);
    absystep = ABS (array->ystep);
    xlen = (Sint32) array->xlen - absxstep;

    switch (bpp)
    {
    case 1:
        while (posy != array->ylen)
        {
            /* Construct the rows */
            PyString_ConcatAndDel (&string, PyString_FromString ("\n  ["));
            posx = 0;
            x = 0;
            while (posx != (Uint32)xlen)
            {
                /* Construct the columns */
                pixel = (Uint32) *((Uint8 *) pixels + array->start + x +
                    y * array->padding);
                PyString_ConcatAndDel (&string, PyString_FromFormat
                    ("%d, ", pixel));
                x += array->xstep;
                posx += absxstep;
            }
            pixel = (Uint32) *((Uint8 *) pixels + array->start + x +
                y * array->padding);
            PyString_ConcatAndDel (&string,
                PyString_FromFormat ("%d]", pixel));
            y += array->ystep;
            posy += absystep;
        }
        break;
    case 2:
        while (posy != array->ylen)
        {
            /* Construct the rows */
            PyString_ConcatAndDel (&string, PyString_FromString ("\n  ["));
            posx = 0;
            x = 0;
            while (posx != (Uint32)xlen)
            {
                /* Construct the columns */
                pixel = (Uint32) *((Uint16 *) (pixels + y * array->padding) +
                    array->start + x);
                PyString_ConcatAndDel (&string, PyString_FromFormat
                    ("%d, ", pixel));
                x += array->xstep;
                posx += absxstep;
            }
            pixel = (Uint32) *((Uint16 *) (pixels + y * array->padding) +
                array->start + x);
            PyString_ConcatAndDel (&string,
                PyString_FromFormat ("%d]", pixel));
            y += array->ystep;
            posy += absystep;
        }
        break;
    case 3:
        while (posy != array->ylen)
        {
            /* Construct the rows */
            PyString_ConcatAndDel (&string, PyString_FromString ("\n  ["));
            posx = 0;
            x = 0;
            while (posx != (Uint32)xlen)
            {
                /* Construct the columns */
                px24 = ((Uint8 *) (pixels + y * array->padding) +
                    array->start * 3 + x * 3);
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
                pixel = (px24[0]) + (px24[1] << 8) + (px24[2] << 16);
#else
                pixel = (px24[2]) + (px24[1] << 8) + (px24[0] << 16);
#endif
                PyString_ConcatAndDel (&string, PyString_FromFormat
                    ("%d, ", pixel));
                x += array->xstep;
                posx += absxstep;
            }
            px24 = ((Uint8 *) (pixels + y * array->padding) +
                array->start * 3 + x * 3);
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
            pixel = (px24[0]) + (px24[1] << 8) + (px24[2] << 16);
#else
            pixel = (px24[2]) + (px24[1] << 8) + (px24[0] << 16);
#endif
            PyString_ConcatAndDel (&string,
                PyString_FromFormat ("%d]", pixel));
            y += array->ystep;
            posy += absystep;
        }
        break;
    default: /* 4bpp */
        while (posy != array->ylen)
        {
            /* Construct the rows */
            PyString_ConcatAndDel (&string, PyString_FromString ("\n  ["));
            posx = 0;
            x = 0;
            while (posx != (Uint32)xlen)
            {
                /* Construct the columns */
                pixel = *((Uint32 *) (pixels + y * array->padding) +
                    array->start + x);
                PyString_ConcatAndDel (&string, PyString_FromFormat
                    ("%d, ", pixel));
                x += array->xstep;
                posx += absxstep;
            }
            pixel = *((Uint32 *) (pixels + y * array->padding) +
                array->start + x);
            PyString_ConcatAndDel (&string,
                PyString_FromFormat ("%d]", pixel));
            y += array->ystep;
            posy += absystep;
        }
        break;
    }
    PyString_ConcatAndDel (&string, PyString_FromString ("\n)"));
    return string;
}

/**
 * Tries to retrieve a valid color for a Surface.
 */
static int
_get_color_from_object (PyObject *val, SDL_PixelFormat *format, Uint32 *color)
{
    Uint8 rgba[4];

    if (PyInt_Check (val))
    {
        int intval = PyInt_AsLong (val);
        if (intval < 0)
        {
            if (!PyErr_Occurred ())
                PyErr_SetString (PyExc_ValueError, "invalid color argument");
            return 0;
        }
        *color = (Uint32) intval;
        return 1;
    }
    else if (PyLong_Check (val))
    {
        long long longval = -1;
        /* Plain index: array[x, */

        longval = PyLong_AsLong (val);
        if ((longval < INT_MIN) || (longval > INT_MAX))
        {
            PyErr_SetString(PyExc_ValueError,
                "index too big for array access");
            return 0;
        }
        *color = (Uint32) longval;
        return 1;
    }
    else if (RGBAFromObj (val, rgba))
    {
        *color = (Uint32) SDL_MapRGBA
            (format, rgba[0], rgba[1], rgba[2], rgba[3]);
        return 1;
    }
    else
        PyErr_SetString (PyExc_ValueError, "invalid color argument");
    return 0;
}

/**
 * Retrieves a single pixel located at index from the surface pixel
 * array.
 */
static PyObject*
_get_single_pixel (Uint8 *pixels, int bpp, Uint32 _index, Uint32 row)
{
    Uint32 pixel;

    switch (bpp)
    {
    case 1:
        pixel = (Uint32)*((Uint8 *) pixels + row + _index);
        break;
    case 2:
        pixel = (Uint32)*((Uint16 *) (pixels + row) + _index);
        break;
    case 3:
    {
        Uint8 *px = ((Uint8 *) (pixels + row) + _index * 3);
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
        pixel = (px[0]) + (px[1] << 8) + (px[2] << 16);
#else
        pixel = (px[2]) + (px[1] << 8) + (px[0] << 16);
#endif
        break;
    }
    default: /* 4 bpp */
        pixel = *((Uint32 *) (pixels + row) + _index);
        break;
    }
    
    return PyInt_FromLong ((long)pixel);
}

/**
 * Sets a single pixel located at index from the surface pixel array.
 */
static void
_set_single_pixel (Uint8 *pixels, int bpp, Uint32 _index, Uint32 row,
    SDL_PixelFormat *format, Uint32 color)
{
    switch (bpp)
    {
    case 1:
        *((Uint8 *) pixels + row + _index) = (Uint8) color;
        break;
    case 2:
        *((Uint16 *) (pixels + row) + _index) = (Uint16) color;
        break;
    case 3:
#if (SDL_BYTEORDER == SDL_LIL_ENDIAN)
        *((Uint8 *) (pixels + row) + _index * 3 + (format->Rshift >> 3)) =
            (Uint8) (color >> 16);
        *((Uint8 *) (pixels + row) + _index * 3 + (format->Gshift >> 3)) =
            (Uint8) (color >> 8);
        *((Uint8 *) (pixels + row) + _index * 3 + (format->Bshift >> 3)) =
            (Uint8) color;
#else
        *((Uint8 *) (pixels + row) + _index * 3 + 2 - (format->Rshift >> 3)) =
            (Uint8) (color >> 16);
        *((Uint8 *) (pixels + row) + _index * 3 + 2 - (format->Gshift >> 3)) =
            (Uint8) (color >> 8);
        *((Uint8 *) (pixels + row) + _index * 3 + 2 - (format->Bshift >> 3)) =
            (Uint8) color;
#endif
        break;
    default: /* 4 bpp */
        *((Uint32 *) (pixels + row) + _index) = color;
        break;
    }
}

/**
 * Creates a 2D slice of the array.
 */
static PyObject*
_array_slice_internal (PyPixelArray *array, Sint32 _start, Sint32 _end,
    Sint32 _step)
{
    Uint32 start;
    Uint32 end;
    Uint32 xlen;
    Uint32 ylen;
    Sint32 xstep;
    Sint32 ystep;
    Uint32 padding;
    SDL_Surface *sf = PySurface_AsSurface (array->surface);

    if (_end == _start)
        return RAISE (PyExc_IndexError, "array size must not be 0");

    GET_SLICE_VALS (array, start, end, ylen, ystep, xlen, xstep, padding,
        _start, _end, _step, sf->pitch);

/*
    printf ("VALS: %d:%d, %d:%d %d:%d %d\n",
        start, end, xlen, xstep, ylen, ystep, padding);
*/
    return (PyObject *) _pxarray_new_internal
        (&PyPixelArray_Type, array->surface, start, end, xlen, ylen,
         xstep, ystep, padding, (PyObject *) array);
}

/**** Sequence interfaces ****/

/**
 * len (array)
 */
static Py_ssize_t
_pxarray_length (PyPixelArray *array)
{
    if (array->xlen > 1)
        return array->xlen / ABS (array->xstep);
    return array->ylen / ABS (array->ystep);
}

/**
 * TODO
 */
static PyObject*
_pxarray_concat (PyPixelArray *array, PyObject *value)
{
    /* TODO */
    return RAISE (PyExc_NotImplementedError, "method not implemented");
}

/**
 * array * 2
 */
static PyObject*
_pxarray_repeat (PyPixelArray *array, Py_ssize_t n)
{
    /* TODO */
    return RAISE (PyExc_NotImplementedError, "method not implemented");
}

/**
 * array[x]
 */
static PyObject*
_pxarray_item (PyPixelArray *array, Py_ssize_t _index)
{
    SDL_Surface *surface;
    int bpp;

    if (_index < 0)
        return RAISE (PyExc_IndexError, "array index out of range");

    surface = PySurface_AsSurface (array->surface);
    bpp = surface->format->BytesPerPixel;

     /* Access of a single column. */
    if (array->xlen == 1)
        return _get_single_pixel ((Uint8 *) surface->pixels, bpp,
            array->start, _index * array->padding * array->ystep);
    if (array->ylen == 1)
        return _get_single_pixel ((Uint8 *) surface->pixels, bpp,
            array->start + _index * array->xstep, _index * array->padding);

    return _array_slice_internal (array, _index, _index + 1, 1);
}

/**
 * array[x:y]
 */
static PyObject*
_pxarray_slice (PyPixelArray *array, Py_ssize_t low, Py_ssize_t high)
{
    if (low < 0)
        low = 0;
    else if (low > (Sint32) array->xlen)
        low = array->xlen;

    if (high < low)
        high = low;
    else if (high > (Sint32) array->xlen)
        high = array->xlen;

    if (low == high)
        Py_RETURN_NONE;

    return _array_slice_internal (array, low, high, 1);
}

static int
_array_assign_array (PyPixelArray *array, Py_ssize_t low, Py_ssize_t high,
    PyPixelArray *val)
{
    SDL_Surface *surface;
    SDL_Surface *valsf = NULL;
    Uint32 x = 0;
    Uint32 y = 0;
    Uint32 vx = 0;
    Uint32 vy = 0;
    int bpp;
    int valbpp;
    Uint8 *pixels;
    Uint8 *assign;
    Py_ssize_t offset = 0;

    Uint32 start;
    Uint32 end;
    Uint32 xlen;
    Uint32 ylen;
    Sint32 xstep;
    Sint32 ystep;
    Uint32 padding;
    Uint32 posx = 0;
    Uint32 posy = 0;
    Sint32 absxstep;
    Sint32 absystep;

    /* Set the correct slice indices */
    surface = PySurface_AsSurface (array->surface);
    GET_SLICE_VALS (array, start, end, ylen, ystep, xlen, xstep, padding,
        low, high, 1, surface->pitch);

    if (val->ylen / val->ystep != ylen / ystep ||
        val->xlen / val->xstep != xlen / xstep)
    {
        /* Bounds do not match. */
        PyErr_SetString (PyExc_ValueError, "array sizes do not match");
        return -1;
    }

    valsf = PySurface_AsSurface (val->surface);
    bpp = surface->format->BytesPerPixel;
    valbpp = valsf->format->BytesPerPixel;
    pixels = (Uint8 *) surface->pixels;

    if (bpp != valbpp)
    {
        /* bpp do not match. We cannot guarantee that the padding and co
         * would be set correctly. */
        PyErr_SetString (PyExc_ValueError, "bit depths do not match");
        return -1;
    }

    absxstep = ABS (xstep);
    absystep = ABS (ystep);

    /* Single value assignment. */
    switch (bpp)
    {
    case 1:
        while (posy != ylen)
        {
            vx = 0;
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                *((Uint8 *) pixels + y * padding + offset) =
                    (Uint8)*((Uint8 *)
                        valsf->pixels + vy * val->padding + val->start + vx);
                vx += val->xstep;
                x += xstep;
                posx += absxstep;
            }
            vy += val->ystep;
            y += ystep;
            posy += absystep;
        }
        break;
    case 2:
        while (posy != ylen)
        {
            vx = 0;
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                *((Uint16 *) (pixels + y * padding) + offset) =
                    (Uint16)*((Uint16 *)
                        ((Uint8*)valsf->pixels + vy * val->padding) +
                        val->start + vx);
                vx += val->xstep;
                x += xstep;
                posx += absxstep;
            }
            vy += val->ystep;
            y += ystep;
            posy += absystep;
        }
        break;
    case 3:
    {
        Uint8 *px;
        Uint8 *vpx;
        SDL_PixelFormat *format = surface->format;
        SDL_PixelFormat *vformat = valsf->format;

        while (posy != ylen)
        {
            vx = 0;
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;

                px = (Uint8 *) (pixels + y * padding) + offset * 3;
                vpx = (Uint8 *) ((Uint8*)valsf->pixels + y * val->padding) +
                    (val->start + vx) * 3;

#if (SDL_BYTEORDER == SDL_LIL_ENDIAN)
                *(px + (format->Rshift >> 3)) =
                    *(vpx + (vformat->Rshift >> 3));
                *(px + (format->Gshift >> 3)) =
                    *(vpx + (vformat->Gshift >> 3));
                *(px + (format->Bshift >> 3)) =
                    *(vpx + (vformat->Bshift >> 3));
#else
                *(px + 2 - (format->Rshift >> 3)) =
                    *(vpx + 2 - (vformat->Rshift >> 3));
                *(px + 2 - (format->Gshift >> 3)) =
                    *(vpx + 2 - (vformat->Gshift >> 3));
                *(px + 2 - (format->Bshift >> 3)) =
                    *(vpx + 2 - (vformat->Bshift >> 3));
#endif
                vx += val->xstep;
                x += xstep;
                posx += absxstep;
            }
            vy += val->ystep;
            y += ystep;
            posy += absystep;
        }
        break;
    }
    default:
        while (posy != ylen)
        {
            vx = 0;
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;

                *((Uint32 *) (pixels + y * padding) + offset) =
                    *((Uint32 *)
                        ((Uint8*)valsf->pixels + y * val->padding) +
                        val->start + vx);
                vx += val->xstep;
                x += xstep;
                posx += absxstep;
            }
            vy += val->ystep;
            y += ystep;
            posy += absystep;
        }
        break;
    }
    return 0;
}

static int
_array_assign_sequence (PyPixelArray *array, Py_ssize_t low, Py_ssize_t high,
    PyObject *val)
{
    SDL_Surface *surface;
    Uint32 x = 0;
    Uint32 y = 0;
    int bpp;
    Uint8 *pixels;
    Uint32 color = 0;
    Uint32 *colorvals = NULL;
    Py_ssize_t offset = 0;
    Py_ssize_t seqsize = PySequence_Size (val);

    Uint32 start;
    Uint32 end;
    Uint32 xlen;
    Uint32 ylen;
    Sint32 xstep;
    Sint32 ystep;
    Uint32 padding;
    Uint32 posx = 0;
    Uint32 posy = 0;
    Sint32 absxstep;
    Sint32 absystep;

    surface = PySurface_AsSurface (array->surface);
    bpp = surface->format->BytesPerPixel;
    pixels = (Uint8 *) surface->pixels;

    /* Set the correct slice indices */
    GET_SLICE_VALS (array, start, end, ylen, ystep, xlen, xstep, padding,
        low, high, 1, surface->pitch);

    if ((Uint32)seqsize != ylen / ystep)
    {
        PyErr_SetString(PyExc_ValueError, "sequence size mismatch");
        return -1;
    }
   
    if (seqsize == 1)
    {
        /* Single value assignment. */
        _set_single_pixel (pixels, bpp, start, x * padding * ystep,
            surface->format, color);
        return 0;
    }

    /* Copy the values. */
    colorvals = malloc (sizeof (Uint32) * seqsize);
    if (!colorvals)
    {
        PyErr_SetString(PyExc_ValueError, "could not copy colors");
        return -1;
    }

    for (offset = 0; offset < seqsize; offset++)
    {
        if (!_get_color_from_object (PySequence_Fast_GET_ITEM (val, offset),
                surface->format, &color))
        {
            free (colorvals);
            return -1;
        }
        colorvals[offset] = color;
    }

    absxstep = ABS (xstep);
    absystep = ABS (ystep);

    switch (bpp)
    {
    case 1:
        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                color = *colorvals++;
                *((Uint8 *) pixels + y * padding + offset) = (Uint8) color;
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    case 2:
        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                color = *colorvals++;
                *((Uint16 *) (pixels + y * padding) + offset) = (Uint16) color;
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    case 3:
    {
        Uint8 *px;
        SDL_PixelFormat *format = surface->format;

        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                color = *colorvals++;
                px = (Uint8 *) (pixels + y * padding) + offset * 3;
#if (SDL_BYTEORDER == SDL_LIL_ENDIAN)
                *(px + (format->Rshift >> 3)) = (Uint8) (color >> 16);
                *(px + (format->Gshift >> 3)) = (Uint8) (color >> 8);
                *(px + (format->Bshift >> 3)) = (Uint8) color;
#else
                *(px + 2 - (format->Rshift >> 3)) = (Uint8) (color >> 16);
                *(px + 2 - (format->Gshift >> 3)) = (Uint8) (color >> 8);
                *(px - (format->Bshift >> 3)) = (Uint8) color;
#endif
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    }
    default:
        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                color = *colorvals++;
                *((Uint32 *) (pixels + y * padding) + offset) = color;
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    }
    return 0;
}

/**
 * array[x] = ...
 */
static int
_pxarray_ass_item (PyPixelArray *array, Py_ssize_t _index, PyObject *value)
{
    SDL_Surface *surface;
    Uint32 x = 0;
    Uint32 y = 0;
    int bpp;
    Uint8 *pixels;
    Uint32 color = 0;
    Py_ssize_t offset = 0;

    Uint32 start;
    Uint32 end;
    Uint32 xlen;
    Uint32 ylen;
    Sint32 xstep;
    Sint32 ystep;
    Uint32 padding;
    Uint32 posx = 0;
    Uint32 posy = 0;
    Sint32 absxstep;
    Sint32 absystep;

    surface = PySurface_AsSurface (array->surface);
    bpp = surface->format->BytesPerPixel;
    pixels = (Uint8 *) surface->pixels;

    if (!_get_color_from_object (value, surface->format, &color))
    {
        if (PyPixelArray_Check (value))
        {
            PyErr_Clear (); /* _get_color_from_object */
            return _array_assign_array (array, _index, _index + 1,
                (PyPixelArray *) value);
        }
        else if (PySequence_Check (value))
        {
            PyErr_Clear (); /* _get_color_from_object */
            return _array_assign_sequence (array, _index, _index + 1, value);
        }
        else /* Error already set by _get_color_from_object(). */
            return -1;
    }

    if (array->xlen == 1) /* Single pixel access. */
    {
        _set_single_pixel (pixels, bpp, array->start,
            _index * array->padding * array->ystep, surface->format, color);
        return 0;
    }

    /* Set the correct slice indices */
    GET_SLICE_VALS (array, start, end, ylen, ystep, xlen, xstep, padding,
        _index, _index + 1, 1, surface->pitch);

/*
    printf ("ITEM: %d:%d, %d:%d %d:%d %d\n",
        start, end, xlen, xstep, ylen, ystep, padding);
*/
    absxstep = ABS (xstep);
    absystep = ABS (ystep);

    /* Single value assignment. */
    switch (bpp)
    {
    case 1:
        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                *((Uint8 *) pixels + y * padding + offset) = (Uint8) color;
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    case 2:
        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                *((Uint16 *) (pixels + y * padding) + offset) = (Uint16) color;
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    case 3:
    {
        Uint8 *px;
        SDL_PixelFormat *format = surface->format;

        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                px = (Uint8 *) (pixels + y * padding) + offset * 3;
#if (SDL_BYTEORDER == SDL_LIL_ENDIAN)
                *(px + (format->Rshift >> 3)) = (Uint8) (color >> 16);
                *(px + (format->Gshift >> 3)) = (Uint8) (color >> 8);
                *(px + (format->Bshift >> 3)) = (Uint8) color;
#else
                *(px + 2 - (format->Rshift >> 3)) = (Uint8) (color >> 16);
                *(px + 2 - (format->Gshift >> 3)) = (Uint8) (color >> 8);
                *(px - (format->Bshift >> 3)) = (Uint8) color;
#endif
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    }
    default: /* 4 bpp */
        while (posy != ylen)
        {
            posx = 0;
            x = 0;
            while (posx != xlen)
            {
                offset = start + x;
                *((Uint32 *) (pixels + y * padding) + offset) = color;
                x += xstep;
                posx += absxstep;
            }
            y += ystep;
            posy += absystep;
        }
        break;
    }
    return 0;
}

/**
 * array[x:y] = ....
 */
static int
_pxarray_ass_slice (PyPixelArray *array, Py_ssize_t low, Py_ssize_t high,
    PyObject *value)
{
    int val = 0;
    int i = 0;

    if (array->xlen != 1)
    {
        if (low < 0)
            low = 0;
        else if (low > (Sint32) array->xlen)
            low = array->xlen;
        
        if (high < low)
            high = low;
        else if (high > (Sint32) array->xlen)
            high = array->xlen;
    }
    else
    {
        if (low < 0)
            low = 0;
        else if (low > (Sint32) array->ylen)
            low = array->ylen;
        
        if (high < low)
            high = low;
        else if (high > (Sint32) array->ylen)
            high = array->ylen;
    }

    if (PyPixelArray_Check (value))
    {
        return _array_assign_array (array, low, high, (PyPixelArray *) value);
    }
    else
    {
        for (i = low; i < high; i++)
        {
            val = _pxarray_ass_item (array, i, value);
            if (val != 0)
                return val;
        }
    }
    return 0;
}

/**
 * x in array
 */
static int
_pxarray_contains (PyPixelArray *array, PyObject *value)
{
    SDL_Surface *surface;
    Uint32 x = 0;
    Uint32 y = 0;
    Py_ssize_t _index;
    Uint8 *pixels;
    int bpp;
    Uint32 color;

    Uint32 posx = 0;
    Uint32 posy = 0;
    Sint32 absxstep;
    Sint32 absystep;

    surface = PySurface_AsSurface (array->surface);
    bpp = surface->format->BytesPerPixel;
    pixels = (Uint8 *) surface->pixels;

    if (!_get_color_from_object (value, surface->format, &color))
    {
        PyErr_SetString (PyExc_TypeError, "invalid color argument");
        return -1;
    }

    absxstep = ABS (array->xstep);
    absystep = ABS (array->ystep);

    switch (bpp)
    {
    case 1:
        while (posy != array->ylen)
        {
            posx = 0;
            x = 0;
            while (posx != array->xlen)
            {
                _index = array->start + x;
                if (*((Uint8 *) pixels + y * array->padding + _index)
                    == (Uint8) color)
                    return 1;
                x += array->xstep;
                posx += absxstep;
            }
            y += array->ystep;
            posy += absystep;
        }
        break;
    case 2:
        while (posy != array->ylen)
        {
            posx = 0;
            x = 0;
            while (posx != array->xlen)
            {
                _index = array->start + x;
                if (*((Uint16 *) (pixels + y * array->padding) + _index)
                    == (Uint16) color)
                    return 1;
                x += array->xstep;
                posx += absxstep;
            }
            y += array->ystep;
            posy += absystep;
        }
        break;
    case 3:
    {
        Uint32 pxcolor;
        Uint8 *pix;

        while (posy != array->ylen)
        {
            posx = 0;
            x = 0;
            while (posx != array->xlen)
            {
                _index = array->start + x;
                pix = ((Uint8 *) (pixels + y * array->padding) + _index * 3);
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
                pxcolor = (pix[0]) + (pix[1] << 8) + (pix[2] << 16);
#else
                pxcolor = (pix[2]) + (pix[1] << 8) + (pix[0] << 16);
#endif
                if (pxcolor == color)
                    return 1;
                x += array->xstep;
                posx += absxstep;
            }
            y += array->ystep;
            posy += absystep;
        }
        break;
    }
    default: /* 4 bpp */
        while (posy != array->ylen)
        {
            posx = 0;
            x = 0;
            while (posx != array->xlen)
            {
                _index = array->start + x;
                if (*((Uint32 *) (pixels + y * array->padding) + _index)
                    == color)
                    return 1;
                x += array->xstep;
                posx += absxstep;
            }
            y += array->ystep;
            posy += absystep;
        }
        break;
    }
    return 0;
}

/**
 * array += ....
 */
static PyObject*
_pxarray_inplace_concat (PyPixelArray *array, PyObject *seq)
{
    /* TODO */
    return RAISE (PyExc_NotImplementedError, "method not implemented");
}

/**
 * array *= ...
 */
static PyObject*
_pxarray_inplace_repeat (PyPixelArray *array, Py_ssize_t n)
{
    /* TODO */
    return RAISE (PyExc_NotImplementedError, "method not implemented");
}

/**** Mapping interfaces ****/

/**
 * Internally used parser function for the 2D slices:
 * array[x,y], array[:,:], ...
 */
static int
_get_subslice (PyPixelArray *array, PyObject *op, Py_ssize_t length,
    Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step)
{
    *start = -1;
    *stop = -1;
    *step = -1;

    if (PyInt_Check (op))
    {
        /* Plain index: array[x, */
        *start = PyInt_AsLong (op);
        *stop = (*start) + 1;
        *step = 1;
    }
    else if (PyLong_Check (op))
    {
        long long val = -1;
        /* Plain index: array[x, */

        val = PyLong_AsLong (op);
        if ((val < INT_MIN) || (val > INT_MAX))
        {
            PyErr_SetString(PyExc_ValueError,
                "index too big for array access");
            return 0;
        }
        *start = (int) val;
        *stop = *start + 1;
        *step = 1;
    }
    else if (PySlice_Check (op))
    {
        Py_ssize_t slicelen;

        /* Operator is a slice: array[x::, */
        if (PySlice_GetIndicesEx ((PySliceObject *) op, length,
                start, stop, step, &slicelen) < 0)
        {
            return 0;
        }
    }
    return 1;
}

/**
 * Slicing support for 1D and 2D access.
 * array[x,y] is only supported for 2D arrays.
 */
static PyObject*
_pxarray_subscript (PyPixelArray *array, PyObject *op)
{
    SDL_Surface *surface = PySurface_AsSurface (array->surface);

    /* Note: order matters here.
     * First check array[x,y], then array[x:y:z], then array[x]
     * Otherwise it'll fail.
     */
    if (PySequence_Check (op))
    {
        PyObject *obj;
        Py_ssize_t size = PySequence_Size (op);
        Py_ssize_t xstart, xstop, xstep;
        Py_ssize_t ystart, ystop, ystep;
        Py_ssize_t lenx, leny;
        
        if (size == 0)
        {
            /* array[,], array[()] ... */
            Py_INCREF (array);
            return (PyObject *) array;
        }
        if (size > 2 || (size == 2 && array->xlen == 1))
            return RAISE (PyExc_IndexError, "too many indices for the array");

        lenx = (array->xlen > 1) ? array->xlen / array->xstep : 0;
        leny = array->ylen / array->ystep;

        obj = PySequence_Fast_GET_ITEM (op, 0);
        if (obj == Py_Ellipsis || obj == Py_None)
        {
            /* Operator is the ellipsis or None
             * array[...,XXX], array[None,XXX]
             */
            xstart = array->start;
            xstop = array->xlen;
            xstep = array->xstep;
        }
        else if (!_get_subslice (array, obj, lenx, &xstart, &xstop, &xstep))
        {
            /* Error on retrieving the subslice. */
            return NULL;
        }

        if (size == 2)
        {
            obj = PySequence_Fast_GET_ITEM (op, 1);
            if (obj == Py_Ellipsis || obj == Py_None)
            {
                /* Operator is the ellipsis or None
                 * array[XXX,...], array[XXX,None]
                 */
                ystart = 0;
                ystop = array->ylen;
                ystep = array->ystep;
            }
            else if (!_get_subslice (array, obj, leny, &ystart, &ystop, &ystep))
            {
                /* Error on retrieving the subslice. */
                return NULL;
            }
        }
        else
        {
            ystart = 0;
            ystop = array->ylen;
            ystep = array->ystep;
        }

        /* Null value? */
        if (xstart == xstop || ystart == ystop)
            Py_RETURN_NONE;

        /* Check boundaries. */
        if (xstart + xstep >= xstop)
        {
            xstop = xstart + 1;
            xstep = 1;
        }

        if (ystart + ystep >= ystop)
        {
            ystop = ystart + 1;
            ystep = 1;
        }

        /* Single value? */
        if (xstart + xstep >= xstop && ystart + ystep >= ystop)
        {
            return  _get_single_pixel ((Uint8 *) surface->pixels,
                surface->format->BytesPerPixel, array->start + xstart,
                ystart * array->padding * array->ystep);
        }

/*
        printf ("X: %d:%d:%d Y: %d:%d:%d\n", xstart, xstop, xstep,
            ystart, ystop, ystep);
*/
/*
        printf ("NEW ARRAY: %d:%d %d:%d:%d %d:%d:%d\n",
            array->start + ystart * array->padding + xstart,
            array->start + ystop * array->padding + xstop,
            xstart, xstop, xstep, ystart, ystop, ystep);
*/
/*
        lenx = MAX((xstop - xstart) / xstep, 1);
        leny = MAX((ystop - ystart) / ystep, 1);
*/

/*
static PyPixelArray* _pxarray_new_internal (PyTypeObject *type,
    PyObject *surface, Uint32 start, Uint32 end, Uint32 xlen, Uint32 ylen,
    Uint32 xstep, Uint32 ystep, Uint32 padding, PyObject *parent);
*/
        return (PyObject *) _pxarray_new_internal (&PyPixelArray_Type,
            array->surface,
            (Uint32) array->start + ystart * array->padding + xstart,
            (Uint32) array->start + ystop * array->padding + xstop,
            (Uint32) xstop - xstart,
            (Uint32) ystop - ystart,
            (Sint32) xstep,
            (Sint32) ystep,
            (Uint32) array->padding, (PyObject *) array);
    }
    else if (PySlice_Check (op))
    {
        /* A slice */
        Py_ssize_t slicelen;
        Py_ssize_t step;
        Py_ssize_t start;
        Py_ssize_t stop;
        int retval;

        if (array->xlen > 1)
        {
            /* 2D array - slice along the x axis */
            retval = PySlice_GetIndicesEx ((PySliceObject *) op,
                (Py_ssize_t) (array->xlen / array->xstep), &start, &stop, &step,
                &slicelen);
        }
        else
        {
            /* 1D array - use the y axis. */
            retval = PySlice_GetIndicesEx ((PySliceObject *) op,
                (Py_ssize_t) (array->ylen / array->ystep), &start, &stop, &step,
                &slicelen);
        }
        if (retval < 0 || slicelen < 0)
            return NULL;
        if (slicelen == 0)
            Py_RETURN_NONE;

/*
        printf ("start: %d, stop: %d, step: %d, len: %d\n", start, stop,
            step, slicelen);
*/
        return (PyObject *) _array_slice_internal (array, start, stop, step);
    }
    else if (PyIndex_Check (op) || PyInt_Check (op) || PyLong_Check (op))
    {
        Py_ssize_t i;
#if PY_VERSION_HEX >= 0x02050000
        PyObject *val = PyNumber_Index (op);
        if (!val)
            return NULL;
        /* A simple index. */
        i = PyNumber_AsSsize_t (val, PyExc_IndexError);
#else
        if (PyInt_Check (op))
            i = PyInt_AsLong (op);
        else
            i = PyLong_AsLong (op);
#endif 
        if (i == -1 && PyErr_Occurred ())
            return NULL;
        if (i < 0)
            i += (array->xlen > 1) ? array->xlen / ABS (array->xstep) :
                array->ylen / ABS (array->ystep);

        return _pxarray_item (array, i);
    }

    return RAISE (PyExc_TypeError,
        "index must be an integer, sequence or slice");
}

static int
_pxarray_ass_subscript (PyPixelArray *array, PyObject* op, PyObject* value)
{
    SDL_Surface *surface = PySurface_AsSurface (array->surface);
    
    if (PyIndex_Check (op) || PyInt_Check (op) || PyLong_Check (op))
    {
        Py_ssize_t i;
#if PY_VERSION_HEX >= 0x02050000
        PyObject *val = PyNumber_Index (op);
        if (!val)
            return -1;
        /* A simple index. */
        i = PyNumber_AsSsize_t (val, PyExc_IndexError);
#else
        if (PyInt_Check (op))
            i = PyInt_AsLong (op);
        else
            i = PyLong_AsLong (op);
#endif 
        if (i == -1 && PyErr_Occurred ())
            return -1;

        if (i < 0)
            i += (array->xlen > 1) ? array->xlen / array->xstep :
                array->ylen / array->ystep;

        return _pxarray_ass_item (array, i, value);
    }
    PyErr_SetString (PyExc_NotImplementedError, "method not implemented");
    return -1;
}

/**** C API interfaces ****/
static PyObject* PyPixelArray_New (PyObject *surfobj)
{
    SDL_Surface *surface;

    if (!PySurface_Check (surfobj))
        return RAISE (PyExc_TypeError, "argument is no a Surface");

    surface = PySurface_AsSurface (surfobj);
    if (surface->format->BytesPerPixel < 1  ||
        surface->format->BytesPerPixel > 4)
        return RAISE (PyExc_ValueError,
            "unsupport bit depth for reference array");

    return (PyObject *) _pxarray_new_internal
        (&PyPixelArray_Type, surfobj, 0, (Uint32) surface->w * surface->h,
            (Uint32) surface->w, (Uint32) surface->h, 1, 1,
            (Uint32) surface->pitch, NULL);
}

PYGAME_EXPORT
void initpixelarray (void)
{
    PyObject *module;
    PyObject *dict;
    PyObject *apiobj;
    static void* c_api[PYGAMEAPI_PIXELARRAY_NUMSLOTS];

    if (PyType_Ready (&PyPixelArray_Type) < 0)
        return;
    
    /* create the module */
    module = Py_InitModule3 ("pixelarray", NULL, NULL);
    PyPixelArray_Type.tp_getattro = PyObject_GenericGetAttr;
    Py_INCREF (&PyPixelArray_Type);
    PyModule_AddObject (module, "PixelArray", (PyObject *) &PyPixelArray_Type);

    dict = PyModule_GetDict (module);

    c_api[0] = &PyPixelArray_Type;
    c_api[1] = PyPixelArray_New;
    apiobj = PyCObject_FromVoidPtr (c_api, NULL);
    PyDict_SetItemString (dict, PYGAMEAPI_LOCAL_ENTRY, apiobj);
    Py_DECREF (apiobj);

    /*imported needed apis*/
    import_pygame_base ();
    import_pygame_surface ();
}