/*
 * Android driver initialisation functions
 *
 * Copyright 1996, 2013, 2017 Alexandre Julliard
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

#define NONAMELESSSTRUCT
#define NONAMELESSUNION
#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <string.h>
#include <link.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "android.h"
#include "wine/server.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

unsigned int screen_width = 0;
unsigned int screen_height = 0;
RECT virtual_screen_rect = { 0, 0, 0, 0 };

MONITORINFOEXW default_monitor =
{
    sizeof(default_monitor),    /* cbSize */
    { 0, 0, 0, 0 },             /* rcMonitor */
    { 0, 0, 0, 0 },             /* rcWork */
    MONITORINFOF_PRIMARY,       /* dwFlags */
    { '\\','\\','.','\\','D','I','S','P','L','A','Y','1',0 }   /* szDevice */
};

static const unsigned int screen_bpp = 32;  /* we don't support other modes */

static int device_init_done;

typedef struct
{
    struct gdi_physdev dev;
} ANDROID_PDEVICE;

static const struct gdi_dc_funcs android_drv_funcs;


/******************************************************************************
 *           init_monitors
 */
void init_monitors( int width, int height )
{
    static const WCHAR trayW[] = {'S','h','e','l','l','_','T','r','a','y','W','n','d',0};
    RECT rect;
    HWND hwnd = FindWindowW( trayW, NULL );

    virtual_screen_rect.right = width;
    virtual_screen_rect.bottom = height;
    default_monitor.rcMonitor = default_monitor.rcWork = virtual_screen_rect;

    if (!hwnd || !IsWindowVisible( hwnd )) return;
    if (!GetWindowRect( hwnd, &rect )) return;
    if (rect.top) default_monitor.rcWork.bottom = rect.top;
    else default_monitor.rcWork.top = rect.bottom;
    TRACE( "found tray %p %s work area %s\n", hwnd,
           wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &default_monitor.rcWork ));
}


/******************************************************************************
 *           set_screen_dpi
 */
void set_screen_dpi( DWORD dpi )
{
    static const WCHAR dpi_key_name[] = {'S','o','f','t','w','a','r','e','\\','F','o','n','t','s',0};
    static const WCHAR dpi_value_name[] = {'L','o','g','P','i','x','e','l','s',0};
    HKEY hkey;

    if (!RegCreateKeyW( HKEY_CURRENT_CONFIG, dpi_key_name, &hkey ))
    {
        RegSetValueExW( hkey, dpi_value_name, 0, REG_DWORD, (void *)&dpi, sizeof(DWORD) );
        RegCloseKey( hkey );
    }
}

void handle_run_cmdline( LPWSTR cmdline, LPWSTR* wineEnv )
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    UNICODE_STRING var, val;
    WCHAR *env = NULL;

    ZeroMemory( &si, sizeof(STARTUPINFOW) );
    TRACE( "Running windows cmd: : %s\n", debugstr_w( cmdline ) );

    if (wineEnv && !RtlCreateEnvironment( TRUE, &env ))
    {
        while (*wineEnv)
        {
            RtlInitUnicodeString( &var, *wineEnv++ );
            RtlInitUnicodeString( &val, *wineEnv++ );
            RtlSetEnvironmentVariable( &env, &var, &val );
        }
    }

    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE,
                         DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT, env, NULL, &si, &pi ))
        ERR( "Failed to run cmd : Error %d\n", GetLastError() );

    if (env) RtlDestroyEnvironment( env );
}

void handle_run_cmdarray( LPWSTR* cmdarray, LPWSTR* wineEnv )
{
    int len = 0;
    int i;
    WCHAR *ptr, *ptr2;
    WCHAR *cmdline;

    ptr = cmdarray[0];
    i = 0;
    while (ptr != NULL)
    {
        len += lstrlenW( ptr ) + 3;
        i++;
        ptr = cmdarray[i];
    }

    cmdline = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );

    ptr = cmdarray[0];
    i = 0;
    ptr2 = cmdline;
    while (ptr != NULL)
    {
        if (ptr[0] != '"')
        {
            *ptr2 = '"';
            ptr2++;
        }
        lstrcpyW(ptr2, ptr);
        ptr2 += lstrlenW(ptr);
        if (ptr[0] != '"')
        {
            *ptr2 = '"';
            ptr2++;
        }
        i++;
        ptr = cmdarray[i];
        if (ptr != NULL)
        {
            *ptr2 = ' ';
            ptr2++;
        }
    }
    *ptr2 = 0;
    handle_run_cmdline( cmdline, wineEnv );
    HeapFree( GetProcessHeap(), 0, cmdline );
}

/**********************************************************************
 *	     fetch_display_metrics
 */
static void fetch_display_metrics(void)
{
    if (wine_get_java_vm()) return;  /* for Java threads it will be set when the top view is created */

    SERVER_START_REQ( get_window_rectangles )
    {
        req->handle = wine_server_user_handle( GetDesktopWindow() );
        req->relative = COORDS_CLIENT;
        if (!wine_server_call( req ))
        {
            screen_width  = reply->window.right;
            screen_height = reply->window.bottom;
        }
    }
    SERVER_END_REQ;

    init_monitors( screen_width, screen_height );
    TRACE( "screen %ux%u\n", screen_width, screen_height );
}


/**********************************************************************
 *           device_init
 *
 * Perform initializations needed upon creation of the first device.
 */
static void device_init(void)
{
    device_init_done = TRUE;
    fetch_display_metrics();
}


/******************************************************************************
 *           create_android_physdev
 */
static ANDROID_PDEVICE *create_android_physdev(void)
{
    ANDROID_PDEVICE *physdev;

    if (!device_init_done) device_init();

    if (!(physdev = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*physdev) ))) return NULL;
    return physdev;
}

/**********************************************************************
 *           ANDROID_CreateDC
 */
static BOOL ANDROID_CreateDC( PHYSDEV *pdev, LPCWSTR driver, LPCWSTR device,
                              LPCWSTR output, const DEVMODEW* initData )
{
    ANDROID_PDEVICE *physdev = create_android_physdev();

    if (!physdev) return FALSE;

    push_dc_driver( pdev, &physdev->dev, &android_drv_funcs );
    return TRUE;
}


/**********************************************************************
 *           ANDROID_CreateCompatibleDC
 */
static BOOL ANDROID_CreateCompatibleDC( PHYSDEV orig, PHYSDEV *pdev )
{
    ANDROID_PDEVICE *physdev = create_android_physdev();

    if (!physdev) return FALSE;

    push_dc_driver( pdev, &physdev->dev, &android_drv_funcs );
    return TRUE;
}


/**********************************************************************
 *           ANDROID_DeleteDC
 */
static BOOL ANDROID_DeleteDC( PHYSDEV dev )
{
    HeapFree( GetProcessHeap(), 0, dev );
    return TRUE;
}


/***********************************************************************
 *           ANDROID_ChangeDisplaySettingsEx
 */
LONG CDECL ANDROID_ChangeDisplaySettingsEx( LPCWSTR devname, LPDEVMODEW devmode,
                                            HWND hwnd, DWORD flags, LPVOID lpvoid )
{
    FIXME( "(%s,%p,%p,0x%08x,%p)\n", debugstr_w( devname ), devmode, hwnd, flags, lpvoid );
    return DISP_CHANGE_SUCCESSFUL;
}


/***********************************************************************
 *           ANDROID_GetMonitorInfo
 */
BOOL CDECL ANDROID_GetMonitorInfo( HMONITOR handle, LPMONITORINFO info )
{
    if (handle != (HMONITOR)1)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    info->rcMonitor = default_monitor.rcMonitor;
    info->rcWork = default_monitor.rcWork;
    info->dwFlags = default_monitor.dwFlags;
    if (info->cbSize >= sizeof(MONITORINFOEXW))
        lstrcpyW( ((MONITORINFOEXW *)info)->szDevice, default_monitor.szDevice );
    return TRUE;
}


/***********************************************************************
 *           ANDROID_EnumDisplayMonitors
 */
BOOL CDECL ANDROID_EnumDisplayMonitors( HDC hdc, LPRECT rect, MONITORENUMPROC proc, LPARAM lp )
{
    return proc( (HMONITOR)1, 0, &default_monitor.rcMonitor, lp );
}


/***********************************************************************
 *           ANDROID_EnumDisplaySettingsEx
 */
BOOL CDECL ANDROID_EnumDisplaySettingsEx( LPCWSTR name, DWORD n, LPDEVMODEW devmode, DWORD flags)
{
    static const WCHAR dev_name[CCHDEVICENAME] =
        { 'W','i','n','e',' ','A','n','d','r','o','i','d',' ','d','r','i','v','e','r',0 };

    devmode->dmSize = offsetof( DEVMODEW, dmICMMethod );
    devmode->dmSpecVersion = DM_SPECVERSION;
    devmode->dmDriverVersion = DM_SPECVERSION;
    memcpy( devmode->dmDeviceName, dev_name, sizeof(dev_name) );
    devmode->dmDriverExtra = 0;
    devmode->u2.dmDisplayFlags = 0;
    devmode->dmDisplayFrequency = 0;
    devmode->u1.s2.dmPosition.x = 0;
    devmode->u1.s2.dmPosition.y = 0;
    devmode->u1.s2.dmDisplayOrientation = 0;
    devmode->u1.s2.dmDisplayFixedOutput = 0;

    if (n == ENUM_CURRENT_SETTINGS || n == ENUM_REGISTRY_SETTINGS) n = 0;
    if (n == 0)
    {
        devmode->dmPelsWidth = screen_width;
        devmode->dmPelsHeight = screen_height;
        devmode->dmBitsPerPel = screen_bpp;
        devmode->dmDisplayFrequency = 60;
        devmode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
        TRACE( "mode %d -- %dx%d %d bpp @%d Hz\n", n,
               devmode->dmPelsWidth, devmode->dmPelsHeight,
               devmode->dmBitsPerPel, devmode->dmDisplayFrequency );
        return TRUE;
    }
    TRACE( "mode %d -- not present\n", n );
    SetLastError( ERROR_NO_MORE_FILES );
    return FALSE;
}


/**********************************************************************
 *           ANDROID_wine_get_wgl_driver
 */
static struct opengl_funcs * ANDROID_wine_get_wgl_driver( PHYSDEV dev, UINT version )
{
    struct opengl_funcs *ret;

    if (!(ret = get_wgl_driver( version )))
    {
        dev = GET_NEXT_PHYSDEV( dev, wine_get_wgl_driver );
        ret = dev->funcs->wine_get_wgl_driver( dev, version );
    }
    return ret;
}


static const struct gdi_dc_funcs android_drv_funcs =
{
    NULL,                               /* pAbortDoc */
    NULL,                               /* pAbortPath */
    NULL,                               /* pAlphaBlend */
    NULL,                               /* pAngleArc */
    NULL,                               /* pArc */
    NULL,                               /* pArcTo */
    NULL,                               /* pBeginPath */
    NULL,                               /* pBlendImage */
    NULL,                               /* pChord */
    NULL,                               /* pCloseFigure */
    ANDROID_CreateCompatibleDC,         /* pCreateCompatibleDC */
    ANDROID_CreateDC,                   /* pCreateDC */
    ANDROID_DeleteDC,                   /* pDeleteDC */
    NULL,                               /* pDeleteObject */
    NULL,                               /* pDeviceCapabilities */
    NULL,                               /* pEllipse */
    NULL,                               /* pEndDoc */
    NULL,                               /* pEndPage */
    NULL,                               /* pEndPath */
    NULL,                               /* pEnumFonts */
    NULL,                               /* pEnumICMProfiles */
    NULL,                               /* pExcludeClipRect */
    NULL,                               /* pExtDeviceMode */
    NULL,                               /* pExtEscape */
    NULL,                               /* pExtFloodFill */
    NULL,                               /* pExtSelectClipRgn */
    NULL,                               /* pExtTextOut */
    NULL,                               /* pFillPath */
    NULL,                               /* pFillRgn */
    NULL,                               /* pFlattenPath */
    NULL,                               /* pFontIsLinked */
    NULL,                               /* pFrameRgn */
    NULL,                               /* pGdiComment */
    NULL,                               /* pGetBoundsRect */
    NULL,                               /* pGetCharABCWidths */
    NULL,                               /* pGetCharABCWidthsI */
    NULL,                               /* pGetCharWidth */
    NULL,                               /* pGetDeviceCaps */
    NULL,                               /* pGetDeviceGammaRamp */
    NULL,                               /* pGetFontData */
    NULL,                               /* pGetFontRealizationInfo */
    NULL,                               /* pGetFontUnicodeRanges */
    NULL,                               /* pGetGlyphIndices */
    NULL,                               /* pGetGlyphOutline */
    NULL,                               /* pGetICMProfile */
    NULL,                               /* pGetImage */
    NULL,                               /* pGetKerningPairs */
    NULL,                               /* pGetNearestColor */
    NULL,                               /* pGetOutlineTextMetrics */
    NULL,                               /* pGetPixel */
    NULL,                               /* pGetSystemPaletteEntries */
    NULL,                               /* pGetTextCharsetInfo */
    NULL,                               /* pGetTextExtentExPoint */
    NULL,                               /* pGetTextExtentExPointI */
    NULL,                               /* pGetTextFace */
    NULL,                               /* pGetTextMetrics */
    NULL,                               /* pGradientFill */
    NULL,                               /* pIntersectClipRect */
    NULL,                               /* pInvertRgn */
    NULL,                               /* pLineTo */
    NULL,                               /* pModifyWorldTransform */
    NULL,                               /* pMoveTo */
    NULL,                               /* pOffsetClipRgn */
    NULL,                               /* pOffsetViewportOrg */
    NULL,                               /* pOffsetWindowOrg */
    NULL,                               /* pPaintRgn */
    NULL,                               /* pPatBlt */
    NULL,                               /* pPie */
    NULL,                               /* pPolyBezier */
    NULL,                               /* pPolyBezierTo */
    NULL,                               /* pPolyDraw */
    NULL,                               /* pPolyPolygon */
    NULL,                               /* pPolyPolyline */
    NULL,                               /* pPolygon */
    NULL,                               /* pPolyline */
    NULL,                               /* pPolylineTo */
    NULL,                               /* pPutImage */
    NULL,                               /* pRealizeDefaultPalette */
    NULL,                               /* pRealizePalette */
    NULL,                               /* pRectangle */
    NULL,                               /* pResetDC */
    NULL,                               /* pRestoreDC */
    NULL,                               /* pRoundRect */
    NULL,                               /* pSaveDC */
    NULL,                               /* pScaleViewportExt */
    NULL,                               /* pScaleWindowExt */
    NULL,                               /* pSelectBitmap */
    NULL,                               /* pSelectBrush */
    NULL,                               /* pSelectClipPath */
    NULL,                               /* pSelectFont */
    NULL,                               /* pSelectPalette */
    NULL,                               /* pSelectPen */
    NULL,                               /* pSetArcDirection */
    NULL,                               /* pSetBkColor */
    NULL,                               /* pSetBkMode */
    NULL,                               /* pSetBoundsRect */
    NULL,                               /* pSetDCBrushColor */
    NULL,                               /* pSetDCPenColor */
    NULL,                               /* pSetDIBitsToDevice */
    NULL,                               /* pSetDeviceClipping */
    NULL,                               /* pSetDeviceGammaRamp */
    NULL,                               /* pSetLayout */
    NULL,                               /* pSetMapMode */
    NULL,                               /* pSetMapperFlags */
    NULL,                               /* pSetPixel */
    NULL,                               /* pSetPolyFillMode */
    NULL,                               /* pSetROP2 */
    NULL,                               /* pSetRelAbs */
    NULL,                               /* pSetStretchBltMode */
    NULL,                               /* pSetTextAlign */
    NULL,                               /* pSetTextCharacterExtra */
    NULL,                               /* pSetTextColor */
    NULL,                               /* pSetTextJustification */
    NULL,                               /* pSetViewportExt */
    NULL,                               /* pSetViewportOrg */
    NULL,                               /* pSetWindowExt */
    NULL,                               /* pSetWindowOrg */
    NULL,                               /* pSetWorldTransform */
    NULL,                               /* pStartDoc */
    NULL,                               /* pStartPage */
    NULL,                               /* pStretchBlt */
    NULL,                               /* pStretchDIBits */
    NULL,                               /* pStrokeAndFillPath */
    NULL,                               /* pStrokePath */
    NULL,                               /* pUnrealizePalette */
    NULL,                               /* pWidenPath */
    ANDROID_wine_get_wgl_driver,        /* wine_get_wgl_driver */
    NULL,                               /* wine_get_vulkan_driver */
    GDI_PRIORITY_GRAPHICS_DRV           /* priority */
};


/******************************************************************************
 *           ANDROID_get_gdi_driver
 */
const struct gdi_dc_funcs * CDECL ANDROID_get_gdi_driver( unsigned int version )
{
    if (version != WINE_GDI_DRIVER_VERSION)
    {
        ERR( "version mismatch, gdi32 wants %u but wineandroid has %u\n", version, WINE_GDI_DRIVER_VERSION );
        return NULL;
    }
    return &android_drv_funcs;
}


static const JNINativeMethod methods[] =
{
    { "wine_desktop_changed", "(II)V", desktop_changed },
    { "wine_config_changed", "(I)V", config_changed },
    { "wine_surface_changed", "(ILandroid/view/Surface;Z)V", surface_changed },
    { "wine_motion_event", "(IIIIII)Z", motion_event },
    { "wine_keyboard_event", "(IIII)Z", keyboard_event },
    { "wine_clear_meta_key_states", "(I)V", clear_meta_key_states },
    { "wine_set_focus", "(I)V", set_focus },
    { "wine_send_syscommand", "(II)V", send_syscommand },
    { "wine_ime_settext", "(Ljava/lang/String;II)V", ime_text},
    { "wine_ime_finishtext", "()V", ime_finish},
    { "wine_ime_canceltext", "()V", ime_cancel},
    { "wine_ime_start", "()V", ime_start},

    { "wine_send_gamepad_count", "(I)V", gamepad_count},
    { "wine_send_gamepad_data", "(IILjava/lang/String;)V", gamepad_data},
    { "wine_send_gamepad_axis", "(I[F)V", gamepad_sendaxis},
    { "wine_send_gamepad_button", "(III)V", gamepad_sendbutton},

    { "wine_clipdata_update", "(I[Ljava/lang/String;)V", clipdata_update },

    { "wine_run_commandline", "(Ljava/lang/String;[Ljava/lang/String;)V", run_commandline },
    { "wine_run_commandarray", "([Ljava/lang/String;[Ljava/lang/String;)V", run_commandarray },
    { "wine_send_window_close", "(I)V", send_window_close}
};

#define DECL_FUNCPTR(f) typeof(f) * p##f = NULL
#define LOAD_FUNCPTR(lib, func) do { \
    if ((p##func = wine_dlsym( lib, #func, NULL, 0 )) == NULL) \
        { ERR( "can't find symbol %s\n", #func); return; } \
    } while(0)

DECL_FUNCPTR( __android_log_print );
DECL_FUNCPTR( ANativeWindow_fromSurface );
DECL_FUNCPTR( ANativeWindow_release );
DECL_FUNCPTR( hw_get_module );

void run_commandline( JNIEnv *env, jobject obj, jobject _cmdline, jobjectArray _wineEnv )
{
    union event_data data;
    const jchar* cmdline = (*env)->GetStringChars(env, _cmdline, 0);
    jsize len = (*env)->GetStringLength( env, _cmdline );
    int j;
    
    memset( &data, 0, sizeof(data) );
    data.type = RUN_CMDLINE;
    data.runcmd.cmdline = malloc( sizeof(WCHAR) * (len + 1) );
    lstrcpynW( data.runcmd.cmdline, (WCHAR*)cmdline, len + 1 );
    (*env)->ReleaseStringChars(env, _cmdline, cmdline);

    if (_wineEnv)
    {
        int count = (*env)->GetArrayLength( env, _wineEnv );

        data.runcmd.env = malloc( sizeof(LPWSTR*) * (count + 1) );
        for (j = 0; j < count; j++)
        {
            jobject s = (*env)->GetObjectArrayElement( env, _wineEnv, j );
            const jchar *key_val_str = (*env)->GetStringChars( env, s, NULL );
            len = (*env)->GetStringLength( env, s );
            data.runcmd.env[j] = malloc( sizeof(WCHAR) * (len + 1) );
            lstrcpynW( data.runcmd.env[j], (WCHAR*)key_val_str, len + 1 );
            (*env)->ReleaseStringChars( env, s, key_val_str );
        }
        data.runcmd.env[j] = 0;
    }

    send_event( &data );
}

void run_commandarray( JNIEnv *env, jobject obj, jobjectArray _cmdarray, jobjectArray _wineEnv )
{
    union event_data data;
    int j;
    jsize len;

    memset( &data, 0, sizeof(data) );
    data.type = RUN_CMDARRAY;

    if (_cmdarray)
    {
        int count = (*env)->GetArrayLength( env, _cmdarray);

        data.runcmdarr.cmdarray = malloc( sizeof(LPWSTR*) * (count + 1) );
        for (j = 0; j < count; j++)
        {
            jobject s = (*env)->GetObjectArrayElement( env, _cmdarray, j );
            const jchar *key_val_str = (*env)->GetStringChars( env, s, NULL );
            len = (*env)->GetStringLength( env, s );
            data.runcmdarr.cmdarray[j] = malloc( sizeof(WCHAR) * (len + 1) );
            lstrcpynW( data.runcmdarr.cmdarray[j], (WCHAR*)key_val_str, len + 1 );
            (*env)->ReleaseStringChars( env, s, key_val_str );
        }
        data.runcmdarr.cmdarray[j] = 0;
    }

    if (_wineEnv)
    {
        int count = (*env)->GetArrayLength( env, _wineEnv );

        data.runcmdarr.env = malloc( sizeof(LPWSTR*) * (count + 1) );
        for (j = 0; j < count; j++)
        {
            jobject s = (*env)->GetObjectArrayElement( env, _wineEnv, j );
            const jchar *key_val_str = (*env)->GetStringChars( env, s, NULL );
            len = (*env)->GetStringLength( env, s );
            data.runcmdarr.env[j] = malloc( sizeof(WCHAR) * (len + 1) );
            lstrcpynW( data.runcmdarr.env[j], (WCHAR*)key_val_str, len + 1 );
            (*env)->ReleaseStringChars( env, s, key_val_str );
        }
        data.runcmdarr.env[j] = 0;
    }

    send_event( &data );
}

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

static unsigned int gnu_hash( const char *name )
{
    unsigned int h = 5381;
    while (*name) h = h * 33 + (unsigned char)*name++;
    return h;
}

static unsigned int hash_symbol( const char *name )
{
    unsigned int hi, hash = 0;
    while (*name)
    {
        hash = (hash << 4) + (unsigned char)*name++;
        hi = hash & 0xf0000000;
        hash ^= hi;
        hash ^= hi >> 24;
    }
    return hash;
}

static void *find_symbol( const struct dl_phdr_info* info, const char *var, int type )
{
    const ElfW(Dyn) *dyn = NULL;
    const ElfW(Phdr) *ph;
    const ElfW(Sym) *symtab = NULL;
    const Elf32_Word *hashtab = NULL;
    const Elf32_Word *gnu_hashtab = NULL;
    const char *strings = NULL;
    Elf32_Word idx;

    for (ph = info->dlpi_phdr; ph < &info->dlpi_phdr[info->dlpi_phnum]; ++ph)
    {
        if (PT_DYNAMIC == ph->p_type)
        {
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + ph->p_vaddr);
            break;
        }
    }
    if (!dyn) return NULL;

    while (dyn->d_tag)
    {
        if (dyn->d_tag == DT_STRTAB)
            strings = (const char*)(info->dlpi_addr + dyn->d_un.d_ptr);
        if (dyn->d_tag == DT_SYMTAB)
            symtab = (const ElfW(Sym) *)(info->dlpi_addr + dyn->d_un.d_ptr);
        if (dyn->d_tag == DT_HASH)
            hashtab = (const Elf32_Word *)(info->dlpi_addr + dyn->d_un.d_ptr);
        if (dyn->d_tag == DT_GNU_HASH)
            gnu_hashtab = (const Elf32_Word *)(info->dlpi_addr + dyn->d_un.d_ptr);
        dyn++;
    }

    if (!symtab || !strings) return NULL;

    if (gnu_hashtab)  /* new style hash table */
    {
        const unsigned int hash   = gnu_hash(var);
        const Elf32_Word nbuckets = gnu_hashtab[0];
        const Elf32_Word symbias  = gnu_hashtab[1];
        const Elf32_Word nwords   = gnu_hashtab[2];
        const ElfW(Addr) *bitmask = (const ElfW(Addr) *)(gnu_hashtab + 4);
        const Elf32_Word *buckets = (const Elf32_Word *)(bitmask + nwords);
        const Elf32_Word *chains  = buckets + nbuckets - symbias;

        if (!(idx = buckets[hash % nbuckets])) return NULL;
        do
        {
            if ((chains[idx] & ~1u) == (hash & ~1u) &&
                ELF32_ST_BIND(symtab[idx].st_info) == STB_GLOBAL &&
                ELF32_ST_TYPE(symtab[idx].st_info) == type &&
                !strcmp( strings + symtab[idx].st_name, var ))
                return (void *)(info->dlpi_addr + symtab[idx].st_value);
        } while (!(chains[idx++] & 1u));
    }
    else if (hashtab)  /* old style hash table */
    {
        const unsigned int hash   = hash_symbol( var );
        const Elf32_Word nbuckets = hashtab[0];
        const Elf32_Word *buckets = hashtab + 2;
        const Elf32_Word *chains  = buckets + nbuckets;

        for (idx = buckets[hash % nbuckets]; idx; idx = chains[idx])
        {
            if (ELF32_ST_BIND(symtab[idx].st_info) == STB_GLOBAL &&
                ELF32_ST_TYPE(symtab[idx].st_info) == type &&
                !strcmp( strings + symtab[idx].st_name, var ))
                return (void *)(info->dlpi_addr + symtab[idx].st_value);
        }
    }
    return NULL;
}

static int enum_libs( struct dl_phdr_info* info, size_t size, void* data )
{
    const char *p;

    if (!info->dlpi_name) return 0;
    if (!(p = strrchr( info->dlpi_name, '/' ))) return 0;
    if (strcmp( p, "/libhardware.so" )) return 0;
    TRACE( "found libhardware at %p\n", info->dlpi_phdr );
    phw_get_module = find_symbol( info, "hw_get_module", STT_FUNC );
    return 1;
}

static void load_hardware_libs(void)
{
    const struct hw_module_t *module;
    int ret;
    void *libhardware;
    char error[256];

    if ((libhardware = wine_dlopen( "libhardware.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        LOAD_FUNCPTR( libhardware, hw_get_module );
    }
    else
    {
        /* Android >= N disallows loading libhardware, so we load libandroid (which imports
         * libhardware), and then we can find libhardware in the list of loaded libraries.
         */
        if (!wine_dlopen( "libandroid.so", RTLD_GLOBAL, error, sizeof(error) ))
        {
            ERR( "failed to load libandroid.so: %s\n", error );
            return;
        }
        dl_iterate_phdr( enum_libs, 0 );
        if (!phw_get_module)
        {
            ERR( "failed to find hw_get_module\n" );
            return;
        }
    }

    if ((ret = phw_get_module( GRALLOC_HARDWARE_MODULE_ID, &module )))
    {
        ERR( "failed to load gralloc module err %d\n", ret );
        return;
    }

    init_gralloc( module );
}

static void load_android_libs(void)
{
    void *libandroid, *liblog;
    char error[1024];

    if (!(libandroid = wine_dlopen( "libandroid.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        ERR( "failed to load libandroid.so: %s\n", error );
        return;
    }
    if (!(liblog = wine_dlopen( "liblog.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        ERR( "failed to load liblog.so: %s\n", error );
        return;
    }
    LOAD_FUNCPTR( liblog, __android_log_print );
    LOAD_FUNCPTR( libandroid, ANativeWindow_fromSurface );
    LOAD_FUNCPTR( libandroid, ANativeWindow_release );
}

#undef DECL_FUNCPTR
#undef LOAD_FUNCPTR

static BOOL process_attach(void)
{
    jclass class;
    jobject object = wine_get_java_object();
    JNIEnv *jni_env;
    JavaVM *java_vm;

    load_hardware_libs();

    if ((java_vm = wine_get_java_vm()))  /* running under Java */
    {
#ifdef __i386__
        WORD old_fs = wine_get_fs();
#endif
        load_android_libs();
        (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 );
        class = (*jni_env)->GetObjectClass( jni_env, object );
        (*jni_env)->RegisterNatives( jni_env, class, methods, ARRAY_SIZE( methods ));
        (*jni_env)->DeleteLocalRef( jni_env, class );
#ifdef __i386__
        wine_set_fs( old_fs );  /* the Java VM hijacks %fs for its own purposes, restore it */
#endif
    }
    return TRUE;
}

/***********************************************************************
 *       dll initialisation routine
 */
BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, LPVOID reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls( inst );
        return process_attach();
    }
    return TRUE;
}
