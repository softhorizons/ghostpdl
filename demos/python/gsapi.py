#! /usr/bin/env python3

'''
Python version of the C API in psi/iapi.h, using ctypes.

Overview:

    All functions have the same name as the C function that they wrap.

    Functions raise a GSError exception if the underlying function returned a
    negative error code.

    Functions that don't have out-params return None. Out-params are returned
    directly (using tuples if there are more than one).

    See examples.py for sample usage.

Usage:

    make sodebug
    LD_LIBRARY_PATH=sodebugbin ./demos/python/gsapi.py

    On Windows perform Release build (x64 or Win32).

Requirements:

    Should work on python-2.5+ and python-3.0+, but this might change in
    future.

Limitations as of 2020-07-21:

    Only very limited testing on has been done.

    Tested on Linux, OpenBSD and Windows.

    Only tested with python-3.7 and 2.7.

    We don't provide gsapi_add_fs() or gsapi_remove_fs().

    We only provide display_callback V2, without V3's
    display_adjust_band_height and display_rectangle_request.

'''

import ctypes
import platform
import sys


if platform.system() in ('Linux', 'OpenBSD'):
    _libgs = ctypes.CDLL('libgs.so')

elif platform.system() == 'Windows':
    if sys.maxsize == 2**31 - 1:
        _libgs = ctypes.CDLL('../../bin/gpdldll32.dll')
    elif sys.maxsize == 2**63 - 10:
        _libgs = ctypes.CDLL('../../bin/gpdldll64.dll')
    else:
        raise Exception('Unrecognised sys.maxsize=0x%x' % sys.maxsize)

else:
    raise Exception('Unrecognised platform.system()=%s' % platform.system())


class GSError(Exception):
    def __init__(self, gs_error):
        self.gs_error = gs_error
    def __str__(self):
        return 'Ghostscript exception %i: %s' % (
                self.gs_error,
                _gs_error_text(self.gs_error),
                )

class gsapi_revision_t:
    def __init__(self, product, copyright, revision, revisiondate):
        self.product = product
        self.copyright = copyright
        self.revision = revision
        self.revisiondate = revisiondate
    def __str__(self):
        return 'product=%r copyright=%r revision=%r revisiondate=%r' % (
                self.product,
                self.copyright,
                self.revision,
                self.revisiondate,
                )

def gsapi_revision():
    '''
    Returns (e, r) where <r> is a gsapi_revision_t.
    '''
    # [unicode: we assume that underlying gsapi_revision() returns utf-8
    # strings.]
    _r = _gsapi_revision_t()
    e = _libgs.gsapi_revision(ctypes.byref(_r), ctypes.sizeof(_r))
    if e < 0:
        raise GSError(e)
    r = gsapi_revision_t(
            _r.product.decode('utf-8'),
            _r.copyright.decode('utf-8'),
            _r.revision,
            _r.revisiondate,
            )
    return r


def gsapi_new_instance(caller_handle):
    '''
    Returns (e, instance).
    '''
    instance = ctypes.c_void_p()
    e = _libgs.gsapi_new_instance(
            ctypes.byref(instance),
            ctypes.c_void_p(caller_handle),
            )
    if e < 0:
        raise GSError(e)
    return instance


def gsapi_delete_instance(instance):
    e = _libgs.gsapi_delete_instance(instance)
    if e < 0:
        raise GSError(e)


def gsapi_set_stdio(instance, stdin_fn, stdout_fn, stderr_fn):
    '''
    stdin_fn:
        If not None, will be called with (caller_handle, text, len_)
        where <text> is a ctypes.LP_c_char of length <len_>.

        [todo: wrap this to be easier to use from Python?]

    stdout_fn and stderr_fn:
        If not None, called with (caller_handle, text):
            caller_handle:
                As passed originally to gsapi_new_instance().
            text:
                A Python bytes object.
        Should return the number of bytes of <text> that they handled; for
        convenience None is converted to len(text).
    '''
    # [unicode: we do not do any encoding or decoding; stdin_fn should encode
    # and stdout_fn and stderr_fn should decode. ]
    def make_out(fn):
        if not fn:
            return None
        def out(caller_handle, text, len_):
            text2 = text[:len_]  # converts from ctypes.LP_c_char to bytes.
            ret = fn(caller_handle, text2)
            if ret is None:
                return len_
            return ret
        return _stdio_fn(out)
    def make_in(fn):
        if not fn:
            return None
        return _stdio_fn(fn)

    stdout_fn2 = make_out(stdout_fn)
    stderr_fn2 = make_out(stderr_fn)
    stdin_fn2  = make_in(stdin_fn)
    e = _libgs.gsapi_set_stdio(instance, stdout_fn2, stdout_fn2, stdout_fn2)
    if e < 0:
        raise GSError(e)
    # Need to keep references to call-back functions.
    global _gsapi_set_stdio_refs
    _gsapi_set_stdio_refs = stdin_fn2, stdout_fn2, stderr_fn2


def gsapi_set_poll(instance, poll_fn):
    poll_fn2 = _poll_fn(poll_fn)
    e = _libgs.gsapi_set_poll(instance, poll_fn2)
    if e < 0:
        raise GSError(e)
    global _gsapi_set_poll_refs
    _gsapi_set_poll_refs = poll_fn2


class display_callback:
    def __init__(self,
            version_major = 0,
            version_minor = 0,
            display_open = 0,
            display_preclose = 0,
            display_close = 0,
            display_presize = 0,
            display_size = 0,
            display_sync = 0,
            display_page = 0,
            display_update = 0,
            display_memalloc = 0,
            display_memfree = 0,
            display_separation = 0,
            display_adjust_band_height = 0,
            ):
        self.version_major              = version_major
        self.version_minor              = version_minor
        self.display_open               = display_open
        self.display_preclose           = display_preclose
        self.display_close              = display_close
        self.display_presize            = display_presize
        self.display_size               = display_size
        self.display_sync               = display_sync
        self.display_page               = display_page
        self.display_update             = display_update
        self.display_memalloc           = display_memalloc
        self.display_memfree            = display_memfree
        self.display_separation         = display_separation
        self.display_adjust_band_height = display_adjust_band_height


def gsapi_set_display_callback(instance, callback):
    assert isinstance(callback, display_callback)
    callback2 = _display_callback()
    callback2.size = ctypes.sizeof(callback2)
    # Copy from <callback> into <callback2>.
    for name, type_ in _display_callback._fields_:
        if name == 'size':
            continue
        value = getattr(callback, name)
        value2 = type_(value)
        setattr(callback2, name, value2)

    e = _libgs.gsapi_set_display_callback(instance, ctypes.byref(callback2))
    if e < 0:
        raise GSError(e)
    # Ensure that we keep references to callbacks.
    global _gsapi_set_display_callback_refs
    _gsapi_set_display_callback_refs = callback2


def gsapi_set_default_device_list(instance, list_):
    # [unicode: we assume that underlying gsapi_set_default_device_list() is
    # expecting list_ to be in utf-8 encoding.]
    assert isinstance(list_, str)
    list_2 = list_.encode('utf-8')
    e = _libgs.gsapi_set_default_device_list(instance, list_2, len(list_))
    if e < 0:
        raise GSError(e)


def gsapi_get_default_device_list(instance):
    '''
    Returns (e, list) where <list> is a string.
    '''
    # [unicode: we assume underlying gsapi_get_default_device_list() returns
    # strings encoded as latin-1.]
    list_ = ctypes.POINTER(ctypes.c_char)()
    len_ = ctypes.c_int()
    e = _libgs.gsapi_get_default_device_list(
            instance,
            ctypes.byref(list_),
            ctypes.byref(len_),
            )
    if e < 0:
        raise GSError(e)
    return list_[:len_.value].decode('latin-1')


GS_ARG_ENCODING_LOCAL = 0
GS_ARG_ENCODING_UTF8 = 1
GS_ARG_ENCODING_UTF16LE = 2


def gsapi_set_arg_encoding(instance, encoding):
    assert encoding in (
            GS_ARG_ENCODING_LOCAL,
            GS_ARG_ENCODING_UTF8,
            GS_ARG_ENCODING_UTF16LE,
            )
    e = _libgs.gsapi_set_arg_encoding(instance, encoding)
    if e < 0:
        raise GSError(e)
    if encoding == GS_ARG_ENCODING_LOCAL:
        # This is probably wrong on Windows.
        _encoding = 'utf-8'
    elif encoding == GS_ARG_ENCODING_UTF8:
        _encoding = 'utf-8'
    elif encoding == GS_ARG_ENCODING_UTF16LE:
        _encoding = 'utf-16-le'


def gsapi_init_with_args(instance, args):
    # [unicode: we assume that underlying gsapi_init_with_args()
    # expects strings in args[] to be encoded in encoding set by
    # gsapi_set_arg_encoding().]

    # Create copy of args in format expected by C.
    argc = len(args)
    argv = (_pchar * (argc + 1))()
    for i, arg in enumerate(args):
        enc_arg = arg.encode(_encoding)
        argv[i] = ctypes.create_string_buffer(enc_arg)
    argv[argc] = None

    e = _libgs.gsapi_init_with_args(instance, argc, argv)
    if e < 0:
        raise GSError(e)


def gsapi_run_string_begin(instance, user_errors):
    '''
    Returns (e, exit_code).
    '''
    pexit_code = ctypes.c_int()
    e = _libgs.gsapi_run_string_begin(instance, user_errors, ctypes.byref(pexit_code))
    if e < 0:
        raise GSError(e)
    return pexit_code.value


def gsapi_run_string_continue(instance, str_, user_errors):
    '''
    <str_> should be either a python string or a bytes object. If the former,
    it is converted into a bytes object using utf-8 encoding.

    We don't raise exception for gs_error_NeedInput.

    Returns exit_code.
    '''
    if isinstance(str_, str):
        str_ = str_.encode('utf-8')
    assert isinstance(str_, bytes)
    pexit_code = ctypes.c_int()
    e = _libgs.gsapi_run_string_continue(
            instance,
            str_,
            len(str_),
            user_errors,
            ctypes.byref(pexit_code),
            )
    if e == gs_error_NeedInput.num:
        # This is normal, so we don't raise.
        pass
    elif e < 0:
        raise GSError(e)
    return pexit_code.value


def gsapi_run_string_end(instance, user_errors):
    '''
    Returns (e, exit_code).
    '''
    pexit_code = ctypes.c_int()
    e = _libgs.gsapi_run_string_end(
            instance,
            user_errors,
            ctypes.byref(pexit_code),
            )
    if e < 0:
        raise GSError(e)
    return pexit_code.value


def gsapi_run_string_with_length(instance, str_, length, user_errors):
    '''
    <str_> should be either a python string or a bytes object. If the former,
    it is converted into a bytes object using utf-8 encoding.

    Returns (e, exit_code).
    '''
    e = gsapi_run_string(instance, str_[:length], user_errors)
    if e < 0:
        raise GSError(e)


def gsapi_run_string(instance, str_, user_errors):
    '''
    <str_> should be either a python string or a bytes object. If the former,
    it is converted into a bytes object using utf-8 encoding.

    Returns (e, exit_code).
    '''
    if isinstance(str_, str):
        str_ = str_.encode('utf-8')
    assert isinstance(str_, bytes)
    pexit_code = ctypes.c_int()
    # We use gsapi_run_string_with_length() because str_ might contain zeros.
    e = _libgs.gsapi_run_string_with_length(
            instance,
            str_,
            len(str_),
            user_errors,
            ctypes.byref(pexit_code),
            )
    if e < 0:
        raise GSError(e)
    return pexit_code.value


def gsapi_run_file(instance, filename, user_errors):
    '''
    Returns (e, exit_code).
    '''
    # [unicode: we assume that underlying gsapi_run_file() expects <filename>
    # to be encoded in encoding set by gsapi_set_arg_encoding().]
    pexit_code = ctypes.c_int()
    filename2 = filename.encode(_encoding)
    e = _libgs.gsapi_run_file(instance, filename2, user_errors, ctypes.byref(pexit_code))
    if e < 0:
        raise GSError(e)
    return pexit_code.value


def gsapi_exit(instance):
    e = _libgs.gsapi_exit(instance)
    if e < 0:
        raise GSError(e)


gs_spt_invalid = -1
gs_spt_null    = 0 # void * is NULL.
gs_spt_bool    = 1 # void * is NULL (false) or non-NULL (true).
gs_spt_int     = 2 # void * is a pointer to an int.
gs_spt_float   = 3 # void * is a float *.
gs_spt_name    = 4 # void * is a char *.
gs_spt_string  = 5 # void * is a char *.
gs_spt_long    = 6 # void * is a long *.
gs_spt_i64     = 7 # void * is an int64_t *.
gs_spt_size_t  = 8 # void * is a size_t *.


def gsapi_set_param(instance, param, value):
    # [unicode: we assume that underlying gsapi_set_param() expects <param> and
    # string <value> to be encoded as latin-1.]
    param2 = param.encode('latin-1')
    if 0: pass
    elif isinstance(value, bool):
        type2 = gs_spt_bool
        value2 = ctypes.byref(ctypes.c_bool(value))
    elif isinstance(value, int):
        type2 = gs_spt_i64
        value2 = ctypes.byref(ctypes.c_longlong(value))
    elif isinstance(value, float):
        type2 = gs_spt_float
        value2 = ctypes.byref(ctypes.c_float(value))
    elif isinstance(value, str):
        # We use gs_spt_string, not psapi_spt_name, because the latter doesn't
        # copy the string.
        type2 = gs_spt_string
        value2 = ctypes.c_char_p(value.encode('latin-1'))
    else:
        assert 0, 'unrecognised type: %s' % type(value)
    e = _libgs.gsapi_set_param(instance, type2, param2, value2)
    if e < 0:
        raise GSError(e)


GS_PERMIT_FILE_READING = 0
GS_PERMIT_FILE_WRITING = 1
GS_PERMIT_FILE_CONTROL = 2


def gsapi_add_control_path(instance, type_, path):
    # [unicode: we assume that underlying gsapi_add_control_path() expects
    # <path> to be encoded in encoding set by gsapi_set_arg_encoding().]
    path2 = path.encode(_encoding)
    e = _libgs.gsapi_add_control_path(instance, type_, path2)
    if e < 0:
        raise GSError(e)


def gsapi_remove_control_path(instance, type_, path):
    # [unicode: we assume that underlying gsapi_remove_control_path() expects
    # <path> to be encoded in encoding set by gsapi_set_arg_encoding().]
    path2 = path.encode(_encoding)
    e = _libgs.gsapi_remove_control_path(instance, type_, path2)
    if e < 0:
        raise GSError(e)


def gsapi_purge_control_paths(instance, type_):
    e = _libgs.gsapi_purge_control_paths(instance, type_)
    if e < 0:
        raise GSError(e)


def gsapi_activate_path_control(instance, enable):
    e = _libgs.gsapi_activate_path_control(instance, enable)
    if e < 0:
        raise GSError(e)


def gsapi_is_path_control_active(instance):
    e = _libgs.gsapi_is_path_control_active(instance)
    if e < 0:
        raise GSError(e)



# Implementation details.
#

_Error_num_to_error = dict()
class _Error:
    def __init__(self, num, desc):
        self.num = num
        self.desc = desc
        _Error_num_to_error[self.num] = self

gs_error_ok                 = _Error(   0, 'ok')
gs_error_unknownerror       = _Error(  -1, 'unknown error')
gs_error_dictfull           = _Error(  -2, 'dict full')
gs_error_dictstackoverflow  = _Error(  -3, 'dict stack overflow')
gs_error_dictstackunderflow = _Error(  -4, 'dict stack underflow')
gs_error_execstackoverflow  = _Error(  -5, 'exec stack overflow')
gs_error_interrupt          = _Error(  -6, 'interrupt')
gs_error_invalidaccess      = _Error(  -7, 'invalid access')
gs_error_invalidexit        = _Error(  -8, 'invalid exit')
gs_error_invalidfileaccess  = _Error(  -9, 'invalid fileaccess')
gs_error_invalidfont        = _Error( -10, 'invalid font')
gs_error_invalidrestore     = _Error( -11, 'invalid restore')
gs_error_ioerror            = _Error( -12, 'ioerror')
gs_error_limitcheck         = _Error( -13, 'limit check')
gs_error_nocurrentpoint     = _Error( -14, 'no current point')
gs_error_rangecheck         = _Error( -15, 'range check')
gs_error_stackoverflow      = _Error( -16, 'stack overflow')
gs_error_stackunderflow     = _Error( -17, 'stack underflow')
gs_error_syntaxerror        = _Error( -18, 'syntax error')
gs_error_timeout            = _Error( -19, 'timeout')
gs_error_typecheck          = _Error( -20, 'type check')
gs_error_undefined          = _Error( -21, 'undefined')
gs_error_undefinedfilename  = _Error( -22, 'undefined filename')
gs_error_undefinedresult    = _Error( -23, 'undefined result')
gs_error_unmatchedmark      = _Error( -24, 'unmatched mark')
gs_error_VMerror            = _Error( -25, 'VMerror')

gs_error_configurationerror = _Error( -26, 'configuration error')
gs_error_undefinedresource  = _Error( -27, 'undefined resource')
gs_error_unregistered       = _Error( -28, 'unregistered')
gs_error_invalidcontext     = _Error( -29, 'invalid context')
gs_error_invalidid          = _Error( -30, 'invalid id')

gs_error_hit_detected       = _Error( -99, 'hit detected')
gs_error_Fatal              = _Error(-100, 'Fatal')
gs_error_Quit               = _Error(-101, 'Quit')
gs_error_InterpreterExit    = _Error(-102, 'Interpreter Exit')
gs_error_Remap_Color        = _Error(-103, 'Remap Color')
gs_error_ExecStackUnderflow = _Error(-104, 'Exec Stack Underflow')
gs_error_VMreclaim          = _Error(-105, 'VM reclaim')
gs_error_NeedInput          = _Error(-106, 'Need Input')
gs_error_NeedFile           = _Error(-107, 'Need File')
gs_error_Info               = _Error(-110, 'Info')
gs_error_handled            = _Error(-111, 'handled')

def _gs_error_text(gs_error):
    '''
    Returns text description of <gs_error>. See base/gserrors.h.
    '''
    e = _Error_num_to_error.get(gs_error)
    if e:
        return e.desc
    return 'no error'


# The encoding that we use when passing strings to the underlying gsapi_*() C
# functions. Changed by gsapi_set_arg_encoding().
#
# This default is probably incorrect on Windows.
#
_encoding = 'utf-8'

class _gsapi_revision_t(ctypes.Structure):
    _fields_ = [
            ('product',         ctypes.c_char_p),
            ('copyright',       ctypes.c_char_p),
            ('revision',        ctypes.c_long),
            ('revisiondate',    ctypes.c_long),
            ]


_stdio_fn = ctypes.CFUNCTYPE(
        ctypes.c_int,                   # return
        ctypes.c_void_p,                # caller_handle
        ctypes.POINTER(ctypes.c_char),  # str
        ctypes.c_int,                   # len
        )

_gsapi_set_stdio_refs = None


# ctypes representation of int (*poll_fn)(void* caller_handle).
#
_poll_fn = ctypes.CFUNCTYPE(
        ctypes.c_int,       # return
        ctypes.c_void_p,    # caller_handle
        )

_gsapi_set_poll_refs = None


# ctypes representation of display_callback.
#
class _display_callback(ctypes.Structure):
    _fields_ = [
            ('size', ctypes.c_int),
            ('version_major', ctypes.c_int),
            ('version_minor', ctypes.c_int),
            ('display_open',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            )),
            ('display_preclose',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            )),
            ('display_close',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            )),
            ('display_presize',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_int,       # width
                            ctypes.c_int,       # height
                            ctypes.c_int,       # raster
                            ctypes.c_uint,      # format
                            )),
            ('display_size',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_int,       # width
                            ctypes.c_int,       # height
                            ctypes.c_int,       # raster
                            ctypes.c_uint,      # format
                            ctypes.c_char_p,    # pimage
                            )),
            ('display_sync',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            )),
            ('display_page',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_int,       # copies
                            ctypes.c_int,       # flush
                            )),
            ('display_update',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_int,       # x
                            ctypes.c_int,       # y
                            ctypes.c_int,       # w
                            ctypes.c_int,       # h
                            )),
            ('display_memalloc',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_ulong,     # size
                            )),
            ('display_memfree',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_void_p,    # mem
                            )),
            ('display_separation',
                    ctypes.CFUNCTYPE(ctypes.c_int,
                            ctypes.c_void_p,    # handle
                            ctypes.c_void_p,    # device
                            ctypes.c_int,       # component
                            ctypes.c_char_p,    # component_name
                            ctypes.c_ushort,    # c
                            ctypes.c_ushort,    # m
                            ctypes.c_ushort,    # y
                            ctypes.c_ushort,    # k
                            )),
            ]


_libgs.gsapi_set_display_callback.argtypes = (
        ctypes.c_void_p,                    # instance
        ctypes.POINTER(_display_callback),  # callback
        )


_gsapi_set_display_callback_refs = None


# See:
#
#   https://stackoverflow.com/questions/58598012/ctypes-errors-with-argv
#
_pchar = ctypes.POINTER(ctypes.c_char)
_ppchar = ctypes.POINTER(_pchar)

_libgs.gsapi_init_with_args.argtypes = (
        ctypes.c_void_p,    # instance
        ctypes.c_int,       # argc
        _ppchar,            # argv
        )


if 0:
    # Not implemented yet:
    #   gsapi_add_fs()
    #   gsapi_remove_fs()
    #
    class gsapi_fs_t(ctypes.Structure):
        _fields_ = [
                ('open_file',
                        ctypes.CFUNCTYPE(ctypes.c_int,
                                ctypes.c_pvoid, # const gs_memory_t *mem
                                ctypes.c_pvoid, # secret
                                ctypes.c_char_p,    # fname
                                ctypes.c_char_p,    # mode
                                )),
                ]



if __name__ == '__main__':

    # test
    #

    print('Running some very simple and incomplete tests...')

    print('libgs: %s' % _libgs)

    revision  = gsapi_revision()
    print('libgs.gsapi_revision() ok: %s' % revision)

    instance = gsapi_new_instance(1)
    print('gsapi_new_instance() ok: %s' % instance)

    gsapi_set_arg_encoding(instance, GS_ARG_ENCODING_UTF8)
    print('gsapi_set_arg_encoding() ok.')

    def stdout_fn(caller_handle, bytes_):
        sys.stdout.write(bytes_.decode('latin-1'))
    gsapi_set_stdio(instance, None, stdout_fn, None)
    print('gsapi_set_stdio() ok.')

    d = display_callback()
    gsapi_set_display_callback(instance, d)
    print('gsapi_set_display_callback() ok.')

    gsapi_set_default_device_list(instance, 'bmp256 bmp32b bmpgray bmpmono bmpsep1 bmpsep8 ccr cdeskjet cdj1600 cdj500')
    print('gsapi_set_default_device_list() ok.')

    l = gsapi_get_default_device_list(instance)
    print('gsapi_get_default_device_list() ok: l=%s' % l)

    gsapi_init_with_args(instance, ['gs',])
    print('gsapi_init_with_args() ok')

    for value in 32, True, 3.14, 'hello world':
        gsapi_set_param(instance, "foo", value);
        print('gsapi_set_param() %s ok.' % value)