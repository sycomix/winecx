/*
 * Android pseudo-device handling
 *
 * Copyright 2014-2017 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "winreg.h"
#include "psapi.h"
#include "ddk/wdm.h"
#include "android.h"
#include "wine/server.h"
#include "wine/unicode.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

#ifndef SYNC_IOC_WAIT
#define SYNC_IOC_WAIT _IOW('>', 0, __s32)
#endif

extern NTSTATUS CDECL wine_ntoskrnl_main_loop( HANDLE stop_event );
static HANDLE stop_event;
static HANDLE thread;
static JNIEnv *jni_env;
static HWND capture_window;

#define ANDROIDCONTROLTYPE  ((ULONG)'A')
#define ANDROID_IOCTL(n) CTL_CODE(ANDROIDCONTROLTYPE, n, METHOD_BUFFERED, FILE_READ_ACCESS)

enum android_ioctl
{
    IOCTL_CREATE_WINDOW,
    IOCTL_DESTROY_WINDOW,
    IOCTL_WINDOW_POS_CHANGED,
    IOCTL_SET_WINDOW_PARENT,
    IOCTL_DEQUEUE_BUFFER,
    IOCTL_QUEUE_BUFFER,
    IOCTL_CANCEL_BUFFER,
    IOCTL_QUERY,
    IOCTL_PERFORM,
    IOCTL_SET_SWAP_INT,
    IOCTL_SET_CAPTURE,
    IOCTL_SET_CURSOR,
    IOCTL_SET_WINDOW_FOCUS,
    IOCTL_SET_WINDOW_TEXT,
    IOCTL_SET_WINDOW_ICON,
    IOCTL_GAMEPAD_QUERY,
    IOCTL_IMETEXT,
    IOCTL_IMEFINISH,
    IOCTL_POLL_CLIPDATA,
    IOCTL_GET_CLIPDATA,
    IOCTL_SET_CLIPDATA,
    IOCTL_CLOSE_DESKTOP,
    NB_IOCTLS
};

#define NB_CACHED_BUFFERS 4

struct native_buffer_wrapper;

/* buffer for storing a variable-size native handle inside an ioctl structure */
union native_handle_buffer
{
    native_handle_t handle;
    int space[256];
};

/* data about the native window in the context of the Java process */
struct native_win_data
{
    struct ANativeWindow       *parent;
    struct ANativeWindowBuffer *buffers[NB_CACHED_BUFFERS];
    void                       *mappings[NB_CACHED_BUFFERS];
    HWND                        hwnd;
    BOOL                        opengl;
    int                         generation;
    int                         api;
    int                         buffer_format;
    int                         swap_interval;
    int                         buffer_lru[NB_CACHED_BUFFERS];
};

/* wrapper for a native window in the context of the client (non-Java) process */
struct native_win_wrapper
{
    struct ANativeWindow          win;
    struct native_buffer_wrapper *buffers[NB_CACHED_BUFFERS];
    struct ANativeWindowBuffer   *locked_buffer;
    HWND                          hwnd;
    BOOL                          opengl;
    LONG                          ref;
};

/* wrapper for a native buffer in the context of the client (non-Java) process */
struct native_buffer_wrapper
{
    struct ANativeWindowBuffer buffer;
    LONG                       ref;
    HWND                       hwnd;
    void                      *bits;
    int                        buffer_id;
    int                        generation;
    union native_handle_buffer native_handle;
};

struct ioctl_header
{
    int  hwnd;
    BOOL opengl;
};

struct ioctl_android_create_window
{
    struct ioctl_header hdr;
    int                 parent;
    float               scale;
};

struct ioctl_android_destroy_window
{
    struct ioctl_header hdr;
};

struct ioctl_android_window_pos_changed
{
    struct ioctl_header hdr;
    RECT                window_rect;
    RECT                client_rect;
    RECT                visible_rect;
    int                 style;
    int                 flags;
    int                 after;
    int                 owner;
};

struct ioctl_android_dequeueBuffer
{
    struct ioctl_header hdr;
    int                 win32;
    int                 width;
    int                 height;
    int                 stride;
    int                 format;
    int                 usage;
    int                 buffer_id;
    int                 generation;
    union native_handle_buffer native_handle;
};

struct ioctl_android_queueBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
    int                 generation;
};

struct ioctl_android_cancelBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
    int                 generation;
};

struct ioctl_android_query
{
    struct ioctl_header hdr;
    int                 what;
    int                 value;
};

struct ioctl_android_perform
{
    struct ioctl_header hdr;
    int                 operation;
    int                 args[4];
};

struct ioctl_android_set_swap_interval
{
    struct ioctl_header hdr;
    int                 interval;
};

struct ioctl_android_set_window_parent
{
    struct ioctl_header hdr;
    int                 parent;
    float               scale;
};

struct ioctl_android_set_window_focus
{
    struct ioctl_header hdr;
};

struct ioctl_android_set_window_text
{
    struct ioctl_header hdr;
    WCHAR               text[1];
};

struct ioctl_android_set_window_icon
{
    struct ioctl_header hdr;
    int                 width;
    int                 height;
    int                 bits[1];
};

struct ioctl_android_set_capture
{
    struct ioctl_header hdr;
};

struct ioctl_android_set_cursor
{
    struct ioctl_header hdr;
    int                 id;
    int                 width;
    int                 height;
    int                 hotspotx;
    int                 hotspoty;
    int                 bits[1];
};

struct ioctl_android_gamepad_value
{
    struct ioctl_header hdr;
    int                 index;
    int                 device;
    union
    {
            int          count;
            di_name      name;
            di_value_set value;
    } data;
};

struct ioctl_android_ime_text
{
    struct ioctl_header hdr;
    INT                 target;
    INT                 length;
    INT                 cursor;
    WCHAR               text[1];
};

struct ioctl_android_ime_finish
{
    struct ioctl_header hdr;
    INT                 target;
};

struct ioctl_android_poll_clipdata
{
    struct ioctl_header hdr;
};

struct ioctl_android_get_clipdata
{
    struct ioctl_header hdr;
    DWORD flags;
    DWORD length;
    WCHAR text[1];
};

struct ioctl_android_set_clipdata
{
    struct ioctl_header hdr;
    WCHAR               text[1];
};

struct ioctl_android_close_desktop
{
    struct ioctl_header hdr;
};

static struct gralloc_module_t *gralloc_module;
static struct gralloc1_device *gralloc1_device;
static BOOL gralloc1_caps[GRALLOC1_LAST_CAPABILITY + 1];

static gralloc1_error_t (*gralloc1_retain)( gralloc1_device_t *device, buffer_handle_t buffer );
static gralloc1_error_t (*gralloc1_release)( gralloc1_device_t *device, buffer_handle_t buffer );
static gralloc1_error_t (*gralloc1_lock)( gralloc1_device_t *device, buffer_handle_t buffer,
                                          uint64_t producerUsage, uint64_t consumerUsage,
                                          const gralloc1_rect_t *accessRegion, void **outData,
                                          int32_t acquireFence );
static gralloc1_error_t (*gralloc1_unlock)( gralloc1_device_t *device, buffer_handle_t buffer,
                                            int32_t *outReleaseFence );

static inline BOOL is_in_desktop_process(void)
{
    return thread != NULL;
}

static inline DWORD current_client_id(void)
{
    return HandleToUlong( PsGetCurrentProcessId() );
}

static inline BOOL is_client_in_process(void)
{
    return current_client_id() == GetCurrentProcessId();
}

#ifdef __i386__  /* the Java VM uses %fs/%gs for its own purposes, so we need to wrap the calls */

static WORD orig_fs, java_fs;
static inline void wrap_java_call(void)   { wine_set_fs( java_fs ); }
static inline void unwrap_java_call(void) { wine_set_fs( orig_fs ); }
static inline void init_java_thread( JavaVM *java_vm )
{
    orig_fs = wine_get_fs();
    (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 );
    java_fs = wine_get_fs();
    wine_set_fs( orig_fs );
}

#elif defined(__x86_64__)

#include <asm/prctl.h>
#include <asm/unistd.h>
static void *orig_teb, *java_teb;
static inline int arch_prctl( int func, void *ptr ) { return syscall( __NR_arch_prctl, func, ptr ); }
static inline void wrap_java_call(void)   { arch_prctl( ARCH_SET_GS, java_teb ); }
static inline void unwrap_java_call(void) { arch_prctl( ARCH_SET_GS, orig_teb ); }
static inline void init_java_thread( JavaVM *java_vm )
{
    arch_prctl( ARCH_GET_GS, &orig_teb );
    (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 );
    arch_prctl( ARCH_GET_GS, &java_teb );
    arch_prctl( ARCH_SET_GS, orig_teb );
}

#else
static inline void wrap_java_call(void) { }
static inline void unwrap_java_call(void) { }
static inline void init_java_thread( JavaVM *java_vm ) { (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 ); }
#endif  /* __i386__ */

static struct native_win_data *data_map[65536];

static unsigned int data_map_idx( HWND hwnd, BOOL opengl )
{
    /* window handles are always even, so use low-order bit for opengl flag */
    return LOWORD(hwnd) + !!opengl;
}

static struct native_win_data *get_native_win_data( HWND hwnd, BOOL opengl )
{
    struct native_win_data *data = data_map[data_map_idx( hwnd, opengl )];

    if (data && data->hwnd == hwnd && !data->opengl == !opengl) return data;
    WARN( "unknown win %p opengl %u\n", hwnd, opengl );
    return NULL;
}

static struct native_win_data *get_ioctl_native_win_data( const struct ioctl_header *hdr )
{
    return get_native_win_data( LongToHandle(hdr->hwnd), hdr->opengl );
}

static int get_ioctl_win_parent( HWND parent )
{
    if (parent == GetDesktopWindow()) return 0;
    if (!GetAncestor( parent, GA_PARENT )) return HandleToLong( HWND_MESSAGE );
    return HandleToLong( parent );
}

static void wait_fence_and_close( int fence )
{
    __s32 timeout = 1000;  /* FIXME: should be -1 for infinite timeout */

    if (fence == -1) return;
    ioctl( fence, SYNC_IOC_WAIT, &timeout );
    close( fence );
}

static int duplicate_fd( HANDLE client, int fd )
{
    HANDLE handle, ret = 0;

    if (!wine_server_fd_to_handle( dup(fd), GENERIC_READ | SYNCHRONIZE, 0, &handle ))
        DuplicateHandle( GetCurrentProcess(), handle, client, &ret,
                         DUPLICATE_SAME_ACCESS, FALSE, DUP_HANDLE_CLOSE_SOURCE );

    if (!ret) return -1;
    return HandleToLong( ret );
}

static int map_native_handle( union native_handle_buffer *dest, const native_handle_t *src,
                              HANDLE mapping, HANDLE client )
{
    const size_t size = offsetof( native_handle_t, data[src->numFds + src->numInts] );
    int i;

    if (mapping)  /* only duplicate the mapping handle */
    {
        HANDLE ret = 0;
        if (!DuplicateHandle( GetCurrentProcess(), mapping, client, &ret,
                              DUPLICATE_SAME_ACCESS, FALSE, DUP_HANDLE_CLOSE_SOURCE ))
            return -ENOSPC;
        dest->handle.numFds = 0;
        dest->handle.numInts = 1;
        dest->handle.data[0] = HandleToLong( ret );
        return 0;
    }
    if (is_client_in_process())  /* transfer the actual handle pointer */
    {
        dest->handle.numFds = 0;
        dest->handle.numInts = sizeof(src) / sizeof(int);
        memcpy( dest->handle.data, &src, sizeof(src) );
        return 0;
    }
    if (size > sizeof(*dest)) return -ENOSPC;
    memcpy( dest, src, size );
    /* transfer file descriptors to the client process */
    for (i = 0; i < dest->handle.numFds; i++)
        dest->handle.data[i] = duplicate_fd( client, src->data[i] );
    return 0;
}

static native_handle_t *unmap_native_handle( const native_handle_t *src )
{
    const size_t size = offsetof( native_handle_t, data[src->numFds + src->numInts] );
    native_handle_t *dest;
    int i;

    if (!is_in_desktop_process())
    {
        dest = malloc( size );
        memcpy( dest, src, size );
        /* fetch file descriptors passed from the server process */
        for (i = 0; i < dest->numFds; i++)
            wine_server_handle_to_fd( LongToHandle(src->data[i]), GENERIC_READ | SYNCHRONIZE,
                                      &dest->data[i], NULL );
    }
    else memcpy( &dest, src->data, sizeof(dest) );
    return dest;
}

static void close_native_handle( native_handle_t *handle )
{
    int i;

    for (i = 0; i < handle->numFds; i++) close( handle->data[i] );
    free( handle );
}

/* insert a buffer index at the head of the LRU list */
static void insert_buffer_lru( struct native_win_data *win, int index )
{
    unsigned int i;

    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (win->buffer_lru[i] == index) break;
        if (win->buffer_lru[i] == -1) break;
    }

    assert( i < NB_CACHED_BUFFERS );
    memmove( win->buffer_lru + 1, win->buffer_lru, i * sizeof(win->buffer_lru[0]) );
    win->buffer_lru[0] = index;
}

static int register_buffer( struct native_win_data *win, struct ANativeWindowBuffer *buffer,
                            HANDLE *mapping, int *is_new )
{
    unsigned int i;

    *is_new = 0;
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (win->buffers[i] == buffer) goto done;
        if (!win->buffers[i]) break;
    }

    if (i == NB_CACHED_BUFFERS)
    {
        /* reuse the least recently used buffer */
        i = win->buffer_lru[NB_CACHED_BUFFERS - 1];
        assert( i < NB_CACHED_BUFFERS );

        TRACE( "%p %p evicting buffer %p id %d from cache\n",
               win->hwnd, win->parent, win->buffers[i], i );
        win->buffers[i]->common.decRef( &win->buffers[i]->common );
        if (win->mappings[i]) UnmapViewOfFile( win->mappings[i] );
    }

    win->buffers[i] = buffer;
    win->mappings[i] = NULL;

    if (mapping)
    {
        *mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                       buffer->stride * buffer->height * 4, NULL );
        win->mappings[i] = MapViewOfFile( *mapping, FILE_MAP_READ, 0, 0, 0 );
    }
    buffer->common.incRef( &buffer->common );
    *is_new = 1;
    TRACE( "%p %p %p -> %d\n", win->hwnd, win->parent, buffer, i );

done:
    insert_buffer_lru( win, i );
    return i;
}

static struct ANativeWindowBuffer *get_registered_buffer( struct native_win_data *win, int id )
{
    if (id < 0 || id >= NB_CACHED_BUFFERS || !win->buffers[id])
    {
        ERR( "unknown buffer %d for %p %p\n", id, win->hwnd, win->parent );
        return NULL;
    }
    return win->buffers[id];
}

static void release_native_window( struct native_win_data *data )
{
    unsigned int i;

    if (data->parent) pANativeWindow_release( data->parent );
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (data->buffers[i]) data->buffers[i]->common.decRef( &data->buffers[i]->common );
        if (data->mappings[i]) UnmapViewOfFile( data->mappings[i] );
        data->buffer_lru[i] = -1;
    }
    memset( data->buffers, 0, sizeof(data->buffers) );
    memset( data->mappings, 0, sizeof(data->mappings) );
}

static void free_native_win_data( struct native_win_data *data )
{
    unsigned int idx = data_map_idx( data->hwnd, data->opengl );

    InterlockedCompareExchangePointer( (void **)&capture_window, 0, data->hwnd );
    release_native_window( data );
    HeapFree( GetProcessHeap(), 0, data );
    data_map[idx] = NULL;
}

static struct native_win_data *create_native_win_data( HWND hwnd, BOOL opengl )
{
    unsigned int i, idx = data_map_idx( hwnd, opengl );
    struct native_win_data *data = data_map[idx];

    if (data)
    {
        WARN( "data for %p not freed correctly\n", data->hwnd );
        free_native_win_data( data );
    }
    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) ))) return NULL;
    data->hwnd = hwnd;
    data->opengl = opengl;
    if (!opengl) data->api = NATIVE_WINDOW_API_CPU;
    data->buffer_format = PF_BGRA_8888;
    data_map[idx] = data;
    for (i = 0; i < NB_CACHED_BUFFERS; i++) data->buffer_lru[i] = -1;
    return data;
}

static void CALLBACK register_native_window_callback( ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3 )
{
    HWND hwnd = (HWND)arg1;
    struct ANativeWindow *win = (struct ANativeWindow *)arg2;
    BOOL opengl = arg3;
    struct native_win_data *data = get_native_win_data( hwnd, opengl );

    if (!win) return;  /* do nothing and hold on to the window until we get a new surface */

    if (!data || data->parent == win)
    {
        if (win) pANativeWindow_release( win );
        if (data && win) PostMessageW( hwnd, WM_ANDROID_REFRESH, opengl, 0 );
        TRACE( "%p -> %p win %p (unchanged)\n", hwnd, data, win );
        return;
    }

    release_native_window( data );
    data->parent = win;
    data->generation++;
    if (win)
    {
        wrap_java_call();
        if (data->api) win->perform( win, NATIVE_WINDOW_API_CONNECT, data->api );
        win->perform( win, NATIVE_WINDOW_SET_BUFFERS_FORMAT, data->buffer_format );
        win->setSwapInterval( win, data->swap_interval );
        unwrap_java_call();
        PostMessageW( hwnd, WM_ANDROID_REFRESH, opengl, 0 );
    }
    TRACE( "%p -> %p win %p\n", hwnd, data, win );
}

/* register a native window received from the Java side for use in ioctls */
void register_native_window( HWND hwnd, struct ANativeWindow *win, BOOL opengl )
{
    NtQueueApcThread( thread, register_native_window_callback, (ULONG_PTR)hwnd, (ULONG_PTR)win, opengl );
}

void init_gralloc( const struct hw_module_t *module )
{
    struct hw_device_t *device;
    int ret;

    TRACE( "got module %p ver %u.%u id %s name %s author %s\n",
           module, module->module_api_version >> 8, module->module_api_version & 0xff,
           debugstr_a(module->id), debugstr_a(module->name), debugstr_a(module->author) );

    switch (module->module_api_version >> 8)
    {
    case 0:
        gralloc_module = (struct gralloc_module_t *)module;
        break;
    case 1:
        if (!(ret = module->methods->open( module, GRALLOC_HARDWARE_MODULE_ID, &device )))
        {
            int32_t caps[64];
            uint32_t i, count = ARRAY_SIZE(caps);

            gralloc1_device = (struct gralloc1_device *)device;
            gralloc1_retain  = gralloc1_device->getFunction( gralloc1_device, GRALLOC1_FUNCTION_RETAIN );
            gralloc1_release = gralloc1_device->getFunction( gralloc1_device, GRALLOC1_FUNCTION_RELEASE );
            gralloc1_lock    = gralloc1_device->getFunction( gralloc1_device, GRALLOC1_FUNCTION_LOCK );
            gralloc1_unlock  = gralloc1_device->getFunction( gralloc1_device, GRALLOC1_FUNCTION_UNLOCK );
            TRACE( "got device version %u funcs %p %p %p %p\n", device->version,
                   gralloc1_retain, gralloc1_release, gralloc1_lock, gralloc1_unlock );

            gralloc1_device->getCapabilities( gralloc1_device, &count, caps );
            if (count == ARRAY_SIZE(caps)) ERR( "too many gralloc capabilities\n" );
            for (i = 0; i < count; i++)
                if (caps[i] < ARRAY_SIZE(gralloc1_caps)) gralloc1_caps[caps[i]] = TRUE;
        }
        else ERR( "failed to open gralloc err %d\n", ret );
        break;
    default:
        ERR( "unknown gralloc module version %u\n", module->module_api_version >> 8 );
        break;
    }
}

static int gralloc_grab_buffer( struct ANativeWindowBuffer *buffer )
{
    if (gralloc1_device)
        return gralloc1_retain( gralloc1_device, buffer->handle );
    if (gralloc_module)
        return gralloc_module->registerBuffer( gralloc_module, buffer->handle );
    return -ENODEV;
}

static void gralloc_release_buffer( struct ANativeWindowBuffer *buffer )
{
    if (gralloc1_device) gralloc1_release( gralloc1_device, buffer->handle );
    else if (gralloc_module) gralloc_module->unregisterBuffer( gralloc_module, buffer->handle );

    if (!gralloc1_caps[GRALLOC1_CAPABILITY_RELEASE_IMPLY_DELETE])
        close_native_handle( (native_handle_t *)buffer->handle );
}

static int gralloc_lock( struct ANativeWindowBuffer *buffer, void **bits )
{
    if (gralloc1_device)
    {
        gralloc1_rect_t rect = { 0, 0, buffer->width, buffer->height };
        return gralloc1_lock( gralloc1_device, buffer->handle,
                              GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN |
                              GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN,
                              GRALLOC1_CONSUMER_USAGE_NONE, &rect, bits, -1 );
    }
    if (gralloc_module)
        return gralloc_module->lock( gralloc_module, buffer->handle,
                                     GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                                     0, 0, buffer->width, buffer->height, bits );

    *bits = ((struct native_buffer_wrapper *)buffer)->bits;
    return 0;
}

static void gralloc_unlock( struct ANativeWindowBuffer *buffer )
{
    if (gralloc1_device)
    {
        int fence;
        gralloc1_unlock( gralloc1_device, buffer->handle, &fence );
        wait_fence_and_close( fence );
    }
    else if (gralloc_module) gralloc_module->unlock( gralloc_module, buffer->handle );
}

/* get the capture window stored in the desktop process */
HWND get_capture_window(void)
{
    return capture_window;
}

static NTSTATUS android_error_to_status( int err )
{
    switch (err)
    {
    case 0:            return STATUS_SUCCESS;
    case -ENOMEM:      return STATUS_NO_MEMORY;
    case -ENOSYS:      return STATUS_NOT_SUPPORTED;
    case -EINVAL:      return STATUS_INVALID_PARAMETER;
    case -ENOENT:      return STATUS_INVALID_HANDLE;
    case -EPERM:       return STATUS_ACCESS_DENIED;
    case -ENODEV:      return STATUS_NO_SUCH_DEVICE;
    case -EEXIST:      return STATUS_DUPLICATE_NAME;
    case -EPIPE:       return STATUS_PIPE_DISCONNECTED;
    case -ENODATA:     return STATUS_NO_MORE_FILES;
    case -ETIMEDOUT:   return STATUS_IO_TIMEOUT;
    case -EBADMSG:     return STATUS_INVALID_DEVICE_REQUEST;
    case -EWOULDBLOCK: return STATUS_DEVICE_NOT_READY;
    default:
        FIXME( "unmapped error %d\n", err );
        return STATUS_UNSUCCESSFUL;
    }
}

static int status_to_android_error( NTSTATUS status )
{
    switch (status)
    {
    case STATUS_SUCCESS:                return 0;
    case STATUS_NO_MEMORY:              return -ENOMEM;
    case STATUS_NOT_SUPPORTED:          return -ENOSYS;
    case STATUS_INVALID_PARAMETER:      return -EINVAL;
    case STATUS_BUFFER_OVERFLOW:        return -EINVAL;
    case STATUS_INVALID_HANDLE:         return -ENOENT;
    case STATUS_ACCESS_DENIED:          return -EPERM;
    case STATUS_NO_SUCH_DEVICE:         return -ENODEV;
    case STATUS_DUPLICATE_NAME:         return -EEXIST;
    case STATUS_PIPE_DISCONNECTED:      return -EPIPE;
    case STATUS_NO_MORE_FILES:          return -ENODATA;
    case STATUS_IO_TIMEOUT:             return -ETIMEDOUT;
    case STATUS_INVALID_DEVICE_REQUEST: return -EBADMSG;
    case STATUS_DEVICE_NOT_READY:       return -EWOULDBLOCK;
    default:
        FIXME( "unmapped status %08x\n", status );
        return -EINVAL;
    }
}

static jobject load_java_method( jmethodID *method, const char *name, const char *args )
{
    jobject object = wine_get_java_object();

    if (!*method)
    {
        jclass class;

        wrap_java_call();
        class = (*jni_env)->GetObjectClass( jni_env, object );
        *method = (*jni_env)->GetMethodID( jni_env, class, name, args );
        unwrap_java_call();
        if (!*method)
        {
            FIXME( "method %s not found\n", name );
            return NULL;
        }
    }
    return object;
}

static void create_desktop_window( HWND hwnd )
{
    static jmethodID method;
    jobject object;
    HKEY hkey;
    DWORD scale = 0;

    /* @@ Wine registry key: HKCU\Software\Wine\Android Driver */
    if (!RegOpenKeyA( HKEY_CURRENT_USER, "Software\\Wine\\Android Driver", &hkey ))
    {
        char buffer[16];
        DWORD type, size = sizeof(buffer);
        if (!RegQueryValueExA( hkey, "DPIScaling", 0, &type, (LPBYTE)buffer, &size ) && type == REG_DWORD)
            scale = *(DWORD *)buffer;
        RegCloseKey( hkey );
    }

    if (!(object = load_java_method( &method, "createDesktopWindow", "(II)V" ))) return;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, HandleToLong( hwnd ), scale );
    unwrap_java_call();
}

static NTSTATUS createWindow_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_create_window *res = data;
    struct native_win_data *win_data;
    jstring str;
    WCHAR modpath[MAX_PATH];
    const WCHAR *modname;
    DWORD pid = current_client_id();
    HANDLE process = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid );
    DWORD rc;
    static const WCHAR none[] = {'n','o','n','e',0};

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = create_native_win_data( LongToHandle(res->hdr.hwnd), res->hdr.opengl ))) return STATUS_NO_MEMORY;

    if ((rc = GetModuleFileNameExW( process, NULL, modpath, MAX_PATH )))
    {
        modname = strrchrW( modpath, '\\' );
        modname = modname ? modname + 1 : modpath;
    }
    else
    {
        ERR( "Failed to get client executable name: %d\n", GetLastError() );
        modname = none;
    }

    TRACE( "hwnd %08x opengl %u parent %08x modname %s\n", res->hdr.hwnd, res->hdr.opengl, res->parent,
           wine_dbgstr_w( modname ) );

    if (!(object = load_java_method( &method, "createWindow", "(IZIFILjava/lang/String;)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    str = (*jni_env)->NewString( jni_env, modname, strlenW( modname ) );
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->hdr.opengl, res->parent, res->scale, pid, str );
    (*jni_env)->DeleteLocalRef( jni_env, str );
    unwrap_java_call();
    CloseHandle( process );
    return STATUS_SUCCESS;
}

static NTSTATUS destroyWindow_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_destroy_window *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    win_data = get_ioctl_native_win_data( &res->hdr );

    TRACE( "hwnd %08x opengl %u\n", res->hdr.hwnd, res->hdr.opengl );

    if (!(object = load_java_method( &method, "destroyWindow", "(I)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd );
    unwrap_java_call();
    if (win_data) free_native_win_data( win_data );
    return STATUS_SUCCESS;
}

static NTSTATUS windowPosChanged_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_window_pos_changed *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x win %s client %s visible %s style %08x flags %08x after %08x owner %08x\n",
           res->hdr.hwnd, wine_dbgstr_rect(&res->window_rect), wine_dbgstr_rect(&res->client_rect),
           wine_dbgstr_rect(&res->visible_rect), res->style, res->flags, res->after, res->owner );

    if (!(object = load_java_method( &method, "windowPosChanged", "(IIIIIIIIIIIIIIIII)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->flags, res->after, res->owner, res->style,
                                res->window_rect.left, res->window_rect.top, res->window_rect.right, res->window_rect.bottom,
                                res->client_rect.left, res->client_rect.top, res->client_rect.right, res->client_rect.bottom,
                                res->visible_rect.left, res->visible_rect.top, res->visible_rect.right, res->visible_rect.bottom );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS dequeueBuffer_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ANativeWindow *parent;
    struct ioctl_android_dequeueBuffer *res = data;
    struct native_win_data *win_data;
    struct ANativeWindowBuffer *buffer;
    int fence, ret, is_new;

    if (out_size < sizeof( *res )) return STATUS_BUFFER_OVERFLOW;

    if (in_size < offsetof( struct ioctl_android_dequeueBuffer, native_handle ))
        return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_DEVICE_NOT_READY;

    *ret_size = offsetof( struct ioctl_android_dequeueBuffer, native_handle );
    wrap_java_call();
    ret = parent->dequeueBuffer( parent, &buffer, &fence );
    unwrap_java_call();
    if (!ret)
    {
        HANDLE mapping = 0;

        TRACE( "%08x got buffer %p fence %d\n", res->hdr.hwnd, buffer, fence );
        res->width  = buffer->width;
        res->height = buffer->height;
        res->stride = buffer->stride;
        res->format = buffer->format;
        res->usage  = buffer->usage;
        res->buffer_id = register_buffer( win_data, buffer, res->win32 ? &mapping : NULL, &is_new );
        res->generation = win_data->generation;
        if (is_new)
        {
            HANDLE process = OpenProcess( PROCESS_DUP_HANDLE, FALSE, current_client_id() );
            map_native_handle( &res->native_handle, buffer->handle, mapping, process );
            CloseHandle( process );
            *ret_size = sizeof( *res );
        }
        wait_fence_and_close( fence );
        return STATUS_SUCCESS;
    }
    ERR( "%08x failed %d\n", res->hdr.hwnd, ret );
    return android_error_to_status( ret );
}

static NTSTATUS cancelBuffer_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_cancelBuffer *res = data;
    struct ANativeWindow *parent;
    struct ANativeWindowBuffer *buffer;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_DEVICE_NOT_READY;
    if (res->generation != win_data->generation) return STATUS_SUCCESS;  /* obsolete buffer, ignore */

    if (!(buffer = get_registered_buffer( win_data, res->buffer_id ))) return STATUS_INVALID_HANDLE;

    TRACE( "%08x buffer %p\n", res->hdr.hwnd, buffer );
    wrap_java_call();
    ret = parent->cancelBuffer( parent, buffer, -1 );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS queueBuffer_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_queueBuffer *res = data;
    struct ANativeWindow *parent;
    struct ANativeWindowBuffer *buffer;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_DEVICE_NOT_READY;
    if (res->generation != win_data->generation) return STATUS_SUCCESS;  /* obsolete buffer, ignore */

    if (!(buffer = get_registered_buffer( win_data, res->buffer_id ))) return STATUS_INVALID_HANDLE;

    TRACE( "%08x buffer %p mapping %p\n", res->hdr.hwnd, buffer, win_data->mappings[res->buffer_id] );
    if (win_data->mappings[res->buffer_id])
    {
        void *bits;
        ret = gralloc_lock( buffer, &bits );
        if (ret) return android_error_to_status( ret );
        memcpy( bits, win_data->mappings[res->buffer_id], buffer->stride * buffer->height * 4 );
        gralloc_unlock( buffer );
    }
    wrap_java_call();
    ret = parent->queueBuffer( parent, buffer, -1 );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS query_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_query *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < sizeof(*res)) return STATUS_BUFFER_OVERFLOW;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_DEVICE_NOT_READY;

    *ret_size = sizeof( *res );
    wrap_java_call();
    ret = parent->query( parent, res->what, &res->value );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS perform_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_perform *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret = -ENOENT;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_DEVICE_NOT_READY;

    switch (res->operation)
    {
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] );
        unwrap_java_call();
        if (!ret) win_data->buffer_format = res->args[0];
        break;
    case NATIVE_WINDOW_API_CONNECT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] );
        unwrap_java_call();
        if (!ret) win_data->api = res->args[0];
        break;
    case NATIVE_WINDOW_API_DISCONNECT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] );
        unwrap_java_call();
        if (!ret) win_data->api = 0;
        break;
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_SCALING_MODE:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, (size_t)res->args[0] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0], res->args[1] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0], res->args[1], res->args[2] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] | ((int64_t)res->args[1] << 32) );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
    case NATIVE_WINDOW_UNLOCK_AND_POST:
        wrap_java_call();
        ret = parent->perform( parent, res->operation );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t rect;
        rect.left   = res->args[0];
        rect.top    = res->args[1];
        rect.right  = res->args[2];
        rect.bottom = res->args[3];
        wrap_java_call();
        ret = parent->perform( parent, res->operation, &rect );
        unwrap_java_call();
        break;
    }
    case NATIVE_WINDOW_LOCK:
    default:
        FIXME( "unsupported perform op %d\n", res->operation );
        break;
    }
    return android_error_to_status( ret );
}

static NTSTATUS setSwapInterval_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_set_swap_interval *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;
    win_data->swap_interval = res->interval;

    if (!(parent = win_data->parent)) return STATUS_SUCCESS;
    wrap_java_call();
    ret = parent->setSwapInterval( parent, res->interval );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS setWindowParent_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_window_parent *res = data;
    struct native_win_data *win_data;
    DWORD pid = current_client_id();

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x parent %08x\n", res->hdr.hwnd, res->parent );

    if (!(object = load_java_method( &method, "setParent", "(IIFI)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->parent, res->scale, pid );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowFocus_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_window_focus *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x\n", res->hdr.hwnd );

    if (!(object = load_java_method( &method, "setFocus", "(I)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowText_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    jstring str;
    int len;
    struct ioctl_android_set_window_text *res = data;
    struct native_win_data *win_data;

    len = in_size - offsetof( struct ioctl_android_set_window_text, text );
    if (len < 0 || (len % sizeof(WCHAR))) return STATUS_INVALID_PARAMETER;
    len /= sizeof(WCHAR);

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x text %s\n", res->hdr.hwnd, wine_dbgstr_wn( res->text, len ));

    if (!(object = load_java_method( &method, "setWindowText", "(ILjava/lang/String;)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    str = (*jni_env)->NewString( jni_env, res->text, len );
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, str );
    (*jni_env)->DeleteLocalRef( jni_env, str );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowIcon_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    int size;
    struct ioctl_android_set_window_icon *res = data;
    struct native_win_data *win_data;

    if (in_size < offsetof( struct ioctl_android_set_window_icon, bits )) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return STATUS_INVALID_HANDLE;

    if (res->width < 0 || res->height < 0 || res->width > 256 || res->height > 256)
        return STATUS_INVALID_PARAMETER;

    size = res->width * res->height;
    if (in_size != offsetof( struct ioctl_android_set_window_icon, bits[size] ))
        return STATUS_INVALID_PARAMETER;

    TRACE( "hwnd %08x size %d\n", res->hdr.hwnd, size );

    if (!(object = load_java_method( &method, "setWindowIcon", "(III[I)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();

    if (size)
    {
        jintArray array = (*jni_env)->NewIntArray( jni_env, size );
        (*jni_env)->SetIntArrayRegion( jni_env, array, 0, size, (jint *)res->bits );
        (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->width, res->height, array );
        (*jni_env)->DeleteLocalRef( jni_env, array );
    }
    else (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, 0, 0, 0 );

    unwrap_java_call();

    return STATUS_SUCCESS;
}

static NTSTATUS setCapture_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_set_capture *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (res->hdr.hwnd && !get_ioctl_native_win_data( &res->hdr )) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x\n", res->hdr.hwnd );

    InterlockedExchangePointer( (void **)&capture_window, LongToHandle( res->hdr.hwnd ));
    return STATUS_SUCCESS;
}

static NTSTATUS setCursor_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    int size;
    struct ioctl_android_set_cursor *res = data;

    if (in_size < offsetof( struct ioctl_android_set_cursor, bits )) return STATUS_INVALID_PARAMETER;

    if (res->width < 0 || res->height < 0 || res->width > 256 || res->height > 256)
        return STATUS_INVALID_PARAMETER;

    size = res->width * res->height;
    if (in_size != offsetof( struct ioctl_android_set_cursor, bits[size] ))
        return STATUS_INVALID_PARAMETER;

    TRACE( "hwnd %08x size %d\n", res->hdr.hwnd, size );

    if (!(object = load_java_method( &method, "setCursor", "(IIIII[I)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();

    if (size)
    {
        jintArray array = (*jni_env)->NewIntArray( jni_env, size );
        (*jni_env)->SetIntArrayRegion( jni_env, array, 0, size, (jint *)res->bits );
        (*jni_env)->CallVoidMethod( jni_env, object, method, 0, res->width, res->height,
                                    res->hotspotx, res->hotspoty, array );
        (*jni_env)->DeleteLocalRef( jni_env, array );
    }
    else (*jni_env)->CallVoidMethod( jni_env, object, method, res->id, 0, 0, 0, 0, 0 );

    unwrap_java_call();

    return STATUS_SUCCESS;
}

static NTSTATUS gamepad_query( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_gamepad_value *res = data;

    if (res->device > di_controllers) return STATUS_INVALID_PARAMETER;
    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < sizeof(*res)) return STATUS_BUFFER_OVERFLOW;

    switch (res->index) {
    case 0:  /* Count */
        res->data.count =  di_controllers;
        break;
    case 1: /* name */
        lstrcpynW(res->data.name, di_names[res->device], DI_NAME_LENGTH);
        break;
    case 2: /* values*/
        memcpy(res->data.value, di_value[res->device], sizeof(res->data.value));
        break;
    }
    *ret_size = sizeof(*res);
    return STATUS_SUCCESS;
}

static NTSTATUS imeText_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    WORD length;
    struct ioctl_android_ime_text *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < sizeof(*res) + ((res->length-1) * sizeof(WCHAR))) return STATUS_BUFFER_OVERFLOW;
    if (res->target < 0 || res->target > java_ime_count) return STATUS_INVALID_PARAMETER;
    if (!java_ime_text[res->target]) return STATUS_INVALID_PARAMETER;

    length = min(java_ime_text[res->target]->length, res->length);
    res->length = java_ime_text[res->target]->length;
    lstrcpynW(res->text, java_ime_text[res->target]->text, length);
    res->cursor = java_ime_text[res->target]->cursor_pos;

    *ret_size = sizeof(*res) + (length * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static NTSTATUS imeFinish_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_ime_finish *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (res->target < 0 || res->target > java_ime_count) return STATUS_INVALID_PARAMETER;

    if (java_ime_text[res->target])
    {
        if (java_ime_text[res->target]->text)
            free(java_ime_text[res->target]->text);
        free(java_ime_text[res->target]);
        java_ime_text[res->target] = NULL;
    }
    *ret_size = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS getClipdata_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_get_clipdata *res = data;
    static jmethodID method;
    jobject object;
    jobject str;
    const jchar *chars;
    jsize length;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < offsetof( struct ioctl_android_get_clipdata, text[res->length] ))
        return STATUS_BUFFER_OVERFLOW;

    if (!(object = load_java_method( &method, "getClipdata", "(I)Ljava/lang/String;" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    str = (*jni_env)->CallObjectMethod( jni_env, object, method, res->flags );
    length = (*jni_env)->GetStringLength( jni_env, str ) + 1;
    if (length <= res->length)
    {
        chars = (*jni_env)->GetStringChars( jni_env, str, NULL );
        memcpy( res->text, chars, sizeof(WCHAR) * (length - 1) );
        res->text[length - 1] = 0;
        (*jni_env)->ReleaseStringChars( jni_env, str, chars );
    }
    res->length = length;
    (*jni_env)->DeleteLocalRef( jni_env, str );
    unwrap_java_call();

    *ret_size = min( in_size, offsetof( struct ioctl_android_get_clipdata, text[res->length] ) );
    return STATUS_SUCCESS;
}

static NTSTATUS pollClipdata_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_poll_clipdata *res = data;
    static jmethodID method;
    jobject object;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(object = load_java_method( &method, "pollClipdata", "()V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method );
    unwrap_java_call();
    *ret_size = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS setClipdata_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    jstring str;
    int len;
    struct ioctl_android_set_clipdata *res = data;

    len = in_size - offsetof( struct ioctl_android_set_clipdata, text );
    if (len < 0 || (len % sizeof(WCHAR))) return STATUS_INVALID_PARAMETER;
    len /= sizeof(WCHAR);

    TRACE( "text %s\n", wine_dbgstr_wn( res->text, len ));

    if (!(object = load_java_method( &method, "setClipdata", "(Ljava/lang/String;)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    str = (*jni_env)->NewString( jni_env, res->text, len );
    (*jni_env)->CallVoidMethod( jni_env, object, method, str );
    (*jni_env)->DeleteLocalRef( jni_env, str );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS closeDesktop_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;

    if (!(object = load_java_method( &method, "closeDesktop", "()V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method );
    unwrap_java_call();
    *ret_size = 0;
    return STATUS_SUCCESS;
}

typedef NTSTATUS (*ioctl_func)( void *in, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size );
static const ioctl_func ioctl_funcs[] =
{
    createWindow_ioctl,         /* IOCTL_CREATE_WINDOW */
    destroyWindow_ioctl,        /* IOCTL_DESTROY_WINDOW */
    windowPosChanged_ioctl,     /* IOCTL_WINDOW_POS_CHANGED */
    setWindowParent_ioctl,      /* IOCTL_SET_WINDOW_PARENT */
    dequeueBuffer_ioctl,        /* IOCTL_DEQUEUE_BUFFER */
    queueBuffer_ioctl,          /* IOCTL_QUEUE_BUFFER */
    cancelBuffer_ioctl,         /* IOCTL_CANCEL_BUFFER */
    query_ioctl,                /* IOCTL_QUERY */
    perform_ioctl,              /* IOCTL_PERFORM */
    setSwapInterval_ioctl,      /* IOCTL_SET_SWAP_INT */
    setCapture_ioctl,           /* IOCTL_SET_CAPTURE */
    setCursor_ioctl,            /* IOCTL_SET_CURSOR */
    setWindowFocus_ioctl,       /* IOCTL_SET_WINDOW_FOCUS */
    setWindowText_ioctl,        /* IOCTL_SET_WINDOW_TEXT */
    setWindowIcon_ioctl,        /* IOCTL_SET_WINDOW_ICON */
    gamepad_query,              /* IOCTL_GAMEPAD_QUERY */
    imeText_ioctl,              /* IOCTL_IMETEXT */
    imeFinish_ioctl,            /* IOCTL_IMEFINISH */
    pollClipdata_ioctl,         /* IOCTL_POLL_CLIPDATA */
    getClipdata_ioctl,          /* IOCTL_GET_CLIPDATA */
    setClipdata_ioctl,          /* IOCTL_SET_CLIPDATA */
    closeDesktop_ioctl,         /* IOCTL_CLOSE_DESKTOP */
};

static NTSTATUS WINAPI ioctl_callback( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *irpsp = IoGetCurrentIrpStackLocation( irp );
    DWORD code = (irpsp->Parameters.DeviceIoControl.IoControlCode - ANDROID_IOCTL(0)) >> 2;

    if (code < NB_IOCTLS)
    {
        struct ioctl_header *header = irp->AssociatedIrp.SystemBuffer;
        DWORD in_size = irpsp->Parameters.DeviceIoControl.InputBufferLength;
        ioctl_func func = ioctl_funcs[code];

        if (in_size >= sizeof(*header))
        {
            irp->IoStatus.Information = 0;
            irp->IoStatus.u.Status = func( irp->AssociatedIrp.SystemBuffer, in_size,
                                           irpsp->Parameters.DeviceIoControl.OutputBufferLength,
                                           &irp->IoStatus.Information );
        }
        else irp->IoStatus.u.Status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        FIXME( "ioctl %x not supported\n", irpsp->Parameters.DeviceIoControl.IoControlCode );
        irp->IoStatus.u.Status = STATUS_NOT_SUPPORTED;
    }
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS CALLBACK init_android_driver( DRIVER_OBJECT *driver, UNICODE_STRING *name )
{
    static const WCHAR device_nameW[] = {'\\','D','e','v','i','c','e','\\','W','i','n','e','A','n','d','r','o','i','d',0 };
    static const WCHAR device_linkW[] = {'\\','?','?','\\','W','i','n','e','A','n','d','r','o','i','d',0 };

    UNICODE_STRING nameW, linkW;
    DEVICE_OBJECT *device;
    NTSTATUS status;

    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ioctl_callback;

    RtlInitUnicodeString( &nameW, device_nameW );
    RtlInitUnicodeString( &linkW, device_linkW );

    if ((status = IoCreateDevice( driver, 0, &nameW, 0, 0, FALSE, &device ))) return status;
    return IoCreateSymbolicLink( &linkW, &nameW );
}

static DWORD CALLBACK device_thread( void *arg )
{
    static const WCHAR driver_nameW[] = {'\\','D','r','i','v','e','r','\\','W','i','n','e','A','n','d','r','o','i','d',0 };

    HANDLE start_event = arg;
    UNICODE_STRING nameW;
    NTSTATUS status;
    JavaVM *java_vm;
    DWORD ret;

    TRACE( "starting process %x\n", GetCurrentProcessId() );

    if (!(java_vm = wine_get_java_vm())) return 0;  /* not running under Java */

    init_java_thread( java_vm );

    create_desktop_window( GetDesktopWindow() );

    RtlInitUnicodeString( &nameW, driver_nameW );
    if ((status = IoCreateDriver( &nameW, init_android_driver )))
    {
        FIXME( "failed to create driver error %x\n", status );
        return status;
    }

    stop_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    SetEvent( start_event );

    ret = wine_ntoskrnl_main_loop( stop_event );

    wrap_java_call();
    (*java_vm)->DetachCurrentThread( java_vm );
    unwrap_java_call();
    return ret;
}

void start_android_device(void)
{
    HANDLE handles[2];

    handles[0] = CreateEventW( NULL, TRUE, FALSE, NULL );
    handles[1] = thread = CreateThread( NULL, 0, device_thread, handles[0], 0, NULL );
    WaitForMultipleObjects( 2, handles, FALSE, INFINITE );
    CloseHandle( handles[0] );
}


/* Client-side ioctl support */


static int android_ioctl( enum android_ioctl code, void *in, DWORD in_size, void *out, DWORD *out_size )
{
    static const WCHAR deviceW[] = {'\\','\\','.','\\','W','i','n','e','A','n','d','r','o','i','d',0 };
    static HANDLE device;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    if (!device)
    {
        HANDLE file = CreateFileW( deviceW, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0 );
        if (file == INVALID_HANDLE_VALUE) return -ENOENT;
        if (InterlockedCompareExchangePointer( &device, file, NULL )) CloseHandle( file );
    }

    status = NtDeviceIoControlFile( device, NULL, NULL, NULL, &iosb, ANDROID_IOCTL(code),
                                    in, in_size, out, out_size ? *out_size : 0 );
    if (status == STATUS_FILE_DELETED)
    {
        WARN( "parent process is gone\n" );
        ExitProcess( 1 );
    }
    if (out_size) *out_size = iosb.Information;
    return status_to_android_error( status );
}

static void win_incRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedIncrement( &win->ref );
}

static void win_decRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedDecrement( &win->ref );
}

static void buffer_incRef( struct android_native_base_t *base )
{
    struct native_buffer_wrapper *buffer = (struct native_buffer_wrapper *)base;
    InterlockedIncrement( &buffer->ref );
}

static void buffer_decRef( struct android_native_base_t *base )
{
    struct native_buffer_wrapper *buffer = (struct native_buffer_wrapper *)base;

    if (!InterlockedDecrement( &buffer->ref ))
    {
        if (!is_in_desktop_process()) gralloc_release_buffer( &buffer->buffer );
        if (buffer->bits) UnmapViewOfFile( buffer->bits );
        HeapFree( GetProcessHeap(), 0, buffer );
    }
}

static int dequeueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_dequeueBuffer res;
    DWORD size = sizeof(res);
    int ret, use_win32 = !gralloc_module && !gralloc1_device;

    res.hdr.hwnd = HandleToLong( win->hwnd );
    res.hdr.opengl = win->opengl;
    res.win32 = use_win32;
    ret = android_ioctl( IOCTL_DEQUEUE_BUFFER,
                         &res, offsetof( struct ioctl_android_dequeueBuffer, native_handle ),
                         &res, &size );
    if (ret) return ret;

    /* if we received the native handle, this is a new buffer */
    if (size > offsetof( struct ioctl_android_dequeueBuffer, native_handle ))
    {
        struct native_buffer_wrapper *buf = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*buf) );

        buf->buffer.common.magic   = ANDROID_NATIVE_BUFFER_MAGIC;
        buf->buffer.common.version = sizeof( buf->buffer );
        buf->buffer.common.incRef  = buffer_incRef;
        buf->buffer.common.decRef  = buffer_decRef;
        buf->buffer.width          = res.width;
        buf->buffer.height         = res.height;
        buf->buffer.stride         = res.stride;
        buf->buffer.format         = res.format;
        buf->buffer.usage          = res.usage;
        buf->buffer.handle         = unmap_native_handle( &res.native_handle.handle );
        buf->ref                   = 1;
        buf->hwnd                  = win->hwnd;
        buf->buffer_id             = res.buffer_id;
        buf->generation            = res.generation;
        if (win->buffers[res.buffer_id])
            win->buffers[res.buffer_id]->buffer.common.decRef(&win->buffers[res.buffer_id]->buffer.common);
        win->buffers[res.buffer_id] = buf;

        if (use_win32)
        {
            HANDLE mapping = LongToHandle( res.native_handle.handle.data[0] );
            buf->bits = MapViewOfFile( mapping, FILE_MAP_WRITE, 0, 0, 0 );
            CloseHandle( mapping );
        }
        else if (!is_in_desktop_process())
        {
            if ((ret = gralloc_grab_buffer( &buf->buffer )) < 0)
                WARN( "hwnd %p, buffer %p failed to register %d %s\n",
                      win->hwnd, &buf->buffer, ret, strerror(-ret) );
        }
    }

    *buffer = &win->buffers[res.buffer_id]->buffer;
    *fence = -1;

    TRACE( "hwnd %p, buffer %p %dx%d stride %d fmt %d usage %d fence %d\n",
           win->hwnd, *buffer, res.width, res.height, res.stride, res.format, res.usage, *fence );
    return 0;
}

static int cancelBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct native_buffer_wrapper *buf = (struct native_buffer_wrapper *)buffer;
    struct ioctl_android_cancelBuffer cancel;

    TRACE( "hwnd %p buffer %p %dx%d stride %d fmt %d usage %d fence %d\n",
           win->hwnd, buffer, buffer->width, buffer->height,
           buffer->stride, buffer->format, buffer->usage, fence );
    cancel.buffer_id = buf->buffer_id;
    cancel.generation = buf->generation;
    cancel.hdr.hwnd = HandleToLong( win->hwnd );
    cancel.hdr.opengl = win->opengl;
    wait_fence_and_close( fence );
    return android_ioctl( IOCTL_CANCEL_BUFFER, &cancel, sizeof(cancel), NULL, NULL );
}

static int queueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct native_buffer_wrapper *buf = (struct native_buffer_wrapper *)buffer;
    struct ioctl_android_queueBuffer queue;

    TRACE( "hwnd %p buffer %p %dx%d stride %d fmt %d usage %d fence %d\n",
           win->hwnd, buffer, buffer->width, buffer->height,
           buffer->stride, buffer->format, buffer->usage, fence );
    queue.buffer_id = buf->buffer_id;
    queue.generation = buf->generation;
    queue.hdr.hwnd = HandleToLong( win->hwnd );
    queue.hdr.opengl = win->opengl;
    wait_fence_and_close( fence );
    return android_ioctl( IOCTL_QUEUE_BUFFER, &queue, sizeof(queue), NULL, NULL );
}

static int dequeueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer )
{
    int fence, ret = dequeueBuffer( window, buffer, &fence );

    if (!ret) wait_fence_and_close( fence );
    return ret;
}

static int cancelBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return cancelBuffer( window, buffer, -1 );
}

static int lockBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return 0;  /* nothing to do */
}

static int queueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return queueBuffer( window, buffer, -1 );
}

static int setSwapInterval( struct ANativeWindow *window, int interval )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_set_swap_interval swap;

    TRACE( "hwnd %p interval %d\n", win->hwnd, interval );
    swap.hdr.hwnd = HandleToLong( win->hwnd );
    swap.hdr.opengl = win->opengl;
    swap.interval = interval;
    return android_ioctl( IOCTL_SET_SWAP_INT, &swap, sizeof(swap), NULL, NULL );
}

static int query( const ANativeWindow *window, int what, int *value )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_query query;
    DWORD size = sizeof( query );
    int ret;

    query.hdr.hwnd = HandleToLong( win->hwnd );
    query.hdr.opengl = win->opengl;
    query.what = what;
    ret = android_ioctl( IOCTL_QUERY, &query, sizeof(query), &query, &size );
    TRACE( "hwnd %p what %d got %d -> %p\n", win->hwnd, what, query.value, value );
    if (!ret) *value = query.value;
    return ret;
}

static int perform( ANativeWindow *window, int operation, ... )
{
    static const char * const names[] =
    {
        "SET_USAGE", "CONNECT", "DISCONNECT", "SET_CROP", "SET_BUFFER_COUNT", "SET_BUFFERS_GEOMETRY",
        "SET_BUFFERS_TRANSFORM", "SET_BUFFERS_TIMESTAMP", "SET_BUFFERS_DIMENSIONS", "SET_BUFFERS_FORMAT",
        "SET_SCALING_MODE", "LOCK", "UNLOCK_AND_POST", "API_CONNECT", "API_DISCONNECT",
        "SET_BUFFERS_USER_DIMENSIONS", "SET_POST_TRANSFORM_CROP"
    };

    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_perform perf;
    va_list args;

    perf.hdr.hwnd  = HandleToLong( win->hwnd );
    perf.hdr.opengl = win->opengl;
    perf.operation = operation;
    memset( perf.args, 0, sizeof(perf.args) );

    va_start( args, operation );
    switch (operation)
    {
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
        perf.args[0] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        perf.args[0] = va_arg( args, size_t );
        TRACE( "hwnd %p %s count %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d\n", win->hwnd, names[operation], perf.args[0], perf.args[1] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        perf.args[2] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d %d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2] );
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t *rect = va_arg( args, android_native_rect_t * );
        perf.args[0] = rect->left;
        perf.args[1] = rect->top;
        perf.args[2] = rect->right;
        perf.args[3] = rect->bottom;
        TRACE( "hwnd %p %s rect %d,%d-%d,%d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2], perf.args[3] );
        break;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
    {
        int64_t timestamp = va_arg( args, int64_t );
        perf.args[0] = timestamp;
        perf.args[1] = timestamp >> 32;
        TRACE( "hwnd %p %s arg %08x%08x\n", win->hwnd, names[operation], perf.args[1], perf.args[0] );
        break;
    }
    case NATIVE_WINDOW_LOCK:
    {
        struct ANativeWindowBuffer *buffer;
        struct ANativeWindow_Buffer *buffer_ret = va_arg( args, ANativeWindow_Buffer * );
        ARect *bounds = va_arg( args, ARect * );
        int ret = window->dequeueBuffer_DEPRECATED( window, &buffer );
        if (!ret)
        {
            if ((ret = gralloc_lock( buffer, &buffer_ret->bits )))
            {
                WARN( "gralloc->lock %p failed %d %s\n", win->hwnd, ret, strerror(-ret) );
                window->cancelBuffer( window, buffer, -1 );
            }
        }
        if (!ret)
        {
            buffer_ret->width  = buffer->width;
            buffer_ret->height = buffer->height;
            buffer_ret->stride = buffer->stride;
            buffer_ret->format = buffer->format;
            win->locked_buffer = buffer;
            if (bounds)
            {
                bounds->left   = 0;
                bounds->top    = 0;
                bounds->right  = buffer->width;
                bounds->bottom = buffer->height;
            }
        }
        va_end( args );
        TRACE( "hwnd %p %s bits %p ret %d %s\n", win->hwnd, names[operation], buffer_ret->bits, ret, strerror(-ret) );
        return ret;
    }
    case NATIVE_WINDOW_UNLOCK_AND_POST:
    {
        int ret = -EINVAL;
        if (win->locked_buffer)
        {
            gralloc_unlock( win->locked_buffer );
            ret = window->queueBuffer( window, win->locked_buffer, -1 );
            win->locked_buffer = NULL;
        }
        va_end( args );
        TRACE( "hwnd %p %s ret %d\n", win->hwnd, names[operation], ret );
        return ret;
    }
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
        TRACE( "hwnd %p %s\n", win->hwnd, names[operation] );
        break;
    case NATIVE_WINDOW_SET_POST_TRANSFORM_CROP:
    default:
        FIXME( "unsupported perform hwnd %p op %d %s\n", win->hwnd, operation,
               operation < ARRAY_SIZE( names ) ? names[operation] : "???" );
        break;
    }
    va_end( args );
    return android_ioctl( IOCTL_PERFORM, &perf, sizeof(perf), NULL, NULL );
}

struct ANativeWindow *create_ioctl_window( HWND hwnd, BOOL opengl, float scale )
{
    struct ioctl_android_create_window req;
    struct native_win_wrapper *win = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*win) );

    if (!win) return NULL;

    win->win.common.magic             = ANDROID_NATIVE_WINDOW_MAGIC;
    win->win.common.version           = sizeof(ANativeWindow);
    win->win.common.incRef            = win_incRef;
    win->win.common.decRef            = win_decRef;
    win->win.setSwapInterval          = setSwapInterval;
    win->win.dequeueBuffer_DEPRECATED = dequeueBuffer_DEPRECATED;
    win->win.lockBuffer_DEPRECATED    = lockBuffer_DEPRECATED;
    win->win.queueBuffer_DEPRECATED   = queueBuffer_DEPRECATED;
    win->win.query                    = query;
    win->win.perform                  = perform;
    win->win.cancelBuffer_DEPRECATED  = cancelBuffer_DEPRECATED;
    win->win.dequeueBuffer            = dequeueBuffer;
    win->win.queueBuffer              = queueBuffer;
    win->win.cancelBuffer             = cancelBuffer;
    win->ref  = 1;
    win->hwnd = hwnd;
    win->opengl = opengl;
    TRACE( "-> %p %p opengl=%u\n", win, win->hwnd, opengl );

    req.hdr.hwnd = HandleToLong( win->hwnd );
    req.hdr.opengl = win->opengl;
    req.parent = get_ioctl_win_parent( GetAncestor( hwnd, GA_PARENT ));
    req.scale = scale;
    android_ioctl( IOCTL_CREATE_WINDOW, &req, sizeof(req), NULL, NULL );

    return &win->win;
}

struct ANativeWindow *grab_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    InterlockedIncrement( &win->ref );
    return window;
}

void release_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    unsigned int i;

    if (InterlockedDecrement( &win->ref ) > 0) return;

    TRACE( "%p %p\n", win, win->hwnd );
    for (i = 0; i < ARRAY_SIZE( win->buffers ); i++)
        if (win->buffers[i]) win->buffers[i]->buffer.common.decRef( &win->buffers[i]->buffer.common );

    destroy_ioctl_window( win->hwnd, win->opengl );
    HeapFree( GetProcessHeap(), 0, win );
}

void destroy_ioctl_window( HWND hwnd, BOOL opengl )
{
    struct ioctl_android_destroy_window req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = opengl;
    android_ioctl( IOCTL_DESTROY_WINDOW, &req, sizeof(req), NULL, NULL );
}

int ioctl_window_pos_changed( HWND hwnd, const RECT *window_rect, const RECT *client_rect,
                              const RECT *visible_rect, UINT style, UINT flags, HWND after, HWND owner )
{
    struct ioctl_android_window_pos_changed req;

    req.hdr.hwnd     = HandleToLong( hwnd );
    req.hdr.opengl   = FALSE;
    req.window_rect  = *window_rect;
    req.client_rect  = *client_rect;
    req.visible_rect = *visible_rect;
    req.style        = style;
    req.flags        = flags;
    req.after        = HandleToLong( after );
    req.owner        = HandleToLong( owner );
    return android_ioctl( IOCTL_WINDOW_POS_CHANGED, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_window_parent( HWND hwnd, HWND parent, float scale )
{
    struct ioctl_android_set_window_parent req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    req.parent = get_ioctl_win_parent( parent );
    req.scale = scale;
    return android_ioctl( IOCTL_SET_WINDOW_PARENT, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_window_focus( HWND hwnd )
{
    struct ioctl_android_set_window_focus req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    return android_ioctl( IOCTL_SET_WINDOW_FOCUS, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_window_text( HWND hwnd, const WCHAR *text )
{
    struct ioctl_android_set_window_text *req;
    unsigned int size = offsetof( struct ioctl_android_set_window_text, text[strlenW(text)] );
    int ret;

    if (!(req = HeapAlloc( GetProcessHeap(), 0, size ))) return -ENOMEM;
    req->hdr.hwnd = HandleToLong( hwnd );
    req->hdr.opengl = FALSE;
    memcpy( req->text, text, strlenW( text ) * sizeof(WCHAR) );
    ret = android_ioctl( IOCTL_SET_WINDOW_TEXT, req, size, NULL, NULL );
    HeapFree( GetProcessHeap(), 0, req );
    return ret;
}

int ioctl_set_window_icon( HWND hwnd, int width, int height, const unsigned int *bits )
{
    struct ioctl_android_set_window_icon *req;
    unsigned int size = offsetof( struct ioctl_android_set_window_icon, bits[width * height] );
    int ret;

    if (!(req = HeapAlloc( GetProcessHeap(), 0, size ))) return -ENOMEM;
    req->hdr.hwnd = HandleToLong( hwnd );
    req->hdr.opengl = FALSE;
    req->width    = width;
    req->height   = height;
    memcpy( req->bits, bits, width * height * sizeof(req->bits[0]) );
    ret = android_ioctl( IOCTL_SET_WINDOW_ICON, req, size, NULL, NULL );
    HeapFree( GetProcessHeap(), 0, req );
    return ret;
}

int ioctl_set_capture( HWND hwnd )
{
    struct ioctl_android_set_capture req;

    req.hdr.hwnd  = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    return android_ioctl( IOCTL_SET_CAPTURE, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_cursor( int id, int width, int height,
                      int hotspotx, int hotspoty, const unsigned int *bits )
{
    struct ioctl_android_set_cursor *req;
    unsigned int size = offsetof( struct ioctl_android_set_cursor, bits[width * height] );
    int ret;

    if (!(req = HeapAlloc( GetProcessHeap(), 0, size ))) return -ENOMEM;
    req->hdr.hwnd   = 0;  /* unused */
    req->hdr.opengl = FALSE;
    req->id       = id;
    req->width    = width;
    req->height   = height;
    req->hotspotx = hotspotx;
    req->hotspoty = hotspoty;
    memcpy( req->bits, bits, width * height * sizeof(req->bits[0]) );
    ret = android_ioctl( IOCTL_SET_CURSOR, req, size, NULL, NULL );
    HeapFree( GetProcessHeap(), 0, req );
    return ret;
}

int ioctl_gamepad_query( int index, int device, void* data)
{
    struct ioctl_android_gamepad_value query;
    DWORD size = sizeof( query );
    int ret;

    query.index = index;
    query.device = device;
    ret = android_ioctl( IOCTL_GAMEPAD_QUERY, &query , sizeof(query), &query, &size );
    switch (index)
    {
        case 0:  /* Count */
            *(int*)data = query.data.count;
            break;
        case 1: /* Name */
            lstrcpynW((WCHAR*)data, query.data.name, DI_NAME_LENGTH);
            break;
        case 2: /* Values*/
            memcpy(data, query.data.value, sizeof(query.data.value));
            break;
    }
    return ret;
}

int ioctl_imeText( int target, int *cursor, int *length, WCHAR* string)
{
    struct ioctl_android_ime_text *query;
    DWORD size = sizeof( *query ) + ((*length - 1) * sizeof(WCHAR));
    int ret;

    query = HeapAlloc(GetProcessHeap(), 0, size);

    query->length = (*length);
    query->target = target;
    ret = android_ioctl( IOCTL_IMETEXT, query , size, query, &size );
    lstrcpynW(string, query->text, query->length);
    *length = query->length;
    *cursor = query->cursor;

    HeapFree(GetProcessHeap(), 0, query);

    return ret;
}

int ioctl_imeFinish( int target )
{
    struct ioctl_android_ime_finish *query;
    DWORD size = sizeof( *query );
    int ret;

    query = HeapAlloc(GetProcessHeap(), 0, size);
    query->target = target;
    ret = android_ioctl( IOCTL_IMEFINISH, query , size, query, &size );
    HeapFree(GetProcessHeap(), 0, query);

    return ret;
}

int ioctl_poll_clipdata(void)
{
    struct ioctl_android_poll_clipdata query;
    DWORD size = sizeof(query);

    return android_ioctl( IOCTL_POLL_CLIPDATA, &query, size, &query, &size );
}

int ioctl_get_clipdata( DWORD flags, LPWSTR* result )
{
    struct ioctl_android_get_clipdata *query;
    DWORD size = sizeof(*query);
    DWORD length = 1;
    int ret;

    query = HeapAlloc( GetProcessHeap(), 0, size );
    query->flags = flags;
    query->length = length;
    while (!(ret = android_ioctl( IOCTL_GET_CLIPDATA, query, size, query, &size )) &&
           query->length > length)
    {
        length = query->length;
        size = sizeof(*query) + sizeof(WCHAR) * (length - 1);

        HeapFree( GetProcessHeap(), 0, query );

        query = HeapAlloc( GetProcessHeap(), 0, size );
        query->flags = flags;
        query->length = length;
    }

    if (!ret)
    {
        *result = HeapAlloc( GetProcessHeap(), 0, sizeof(WCHAR) * query->length );
        memcpy( *result, query->text, sizeof(WCHAR) * query->length );
    }

    HeapFree( GetProcessHeap(), 0, query );

    return ret;
}

int ioctl_set_clipdata( const WCHAR *text )
{
    struct ioctl_android_set_clipdata *req;
    unsigned int size;
    int ret;
    static const WCHAR empty[] = { 0 };

    if (!text) text = empty;

    size = offsetof( struct ioctl_android_set_clipdata, text[strlenW(text)] );

    if (!(req = HeapAlloc( GetProcessHeap(), 0, size ))) return -ENOMEM;
    memcpy( req->text, text, strlenW( text ) * sizeof(WCHAR) );
    ret = android_ioctl( IOCTL_SET_CLIPDATA, req, size, NULL, NULL );
    HeapFree( GetProcessHeap(), 0, req );
    return ret;
}

int ioctl_close_desktop(void)
{
    struct ioctl_android_close_desktop *query;
    DWORD size = sizeof( *query );
    int ret;

    query = HeapAlloc(GetProcessHeap(), 0, size);
    ret = android_ioctl( IOCTL_CLOSE_DESKTOP, query , size, query, &size );
    HeapFree(GetProcessHeap(), 0, query);

    return ret;
}
