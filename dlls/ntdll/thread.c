/*
 * NT threads support
 *
 * Copyright 1996, 2003 Alexandre Julliard
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
#include <stdarg.h>
#include <limits.h>
#include <sys/types.h>
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef HAVE_SYS_TIMES_H
#include <sys/times.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

#define NONAMELESSUNION
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "wine/library.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "winbase.h"
#include "ntdll_misc.h"
#include "ddk/wdm.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(thread);

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif

static struct _KUSER_SHARED_DATA user_shared_data_internal;
struct _KUSER_SHARED_DATA *user_shared_data_external;
struct _KUSER_SHARED_DATA *user_shared_data = &user_shared_data_internal;
static const WCHAR default_windirW[] = {'C',':','\\','w','i','n','d','o','w','s',0};

extern void DECLSPEC_NORETURN __wine_syscall_dispatcher( void );

void (WINAPI *kernel32_start_process)(LPTHREAD_START_ROUTINE,void*) = NULL;

/* info passed to a starting thread */
struct startup_info
{
    TEB                            *teb;
    LPTHREAD_START_ROUTINE          entry_point;
    void                           *entry_arg;
};

static PEB *peb;
static PEB_LDR_DATA ldr;
static RTL_USER_PROCESS_PARAMETERS params;  /* default parameters if no parent */
static WCHAR current_dir[MAX_PATH];
static RTL_BITMAP tls_bitmap;
static RTL_BITMAP tls_expansion_bitmap;
static RTL_BITMAP fls_bitmap;
static int nb_threads = 1;

static RTL_CRITICAL_SECTION peb_lock;
static RTL_CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &peb_lock,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": peb_lock") }
};
static RTL_CRITICAL_SECTION peb_lock = { &critsect_debug, -1, 0, 0, 0, 0 };

/***********************************************************************
 *           get_unicode_string
 *
 * Copy a unicode string from the startup info.
 */
static inline void get_unicode_string( UNICODE_STRING *str, WCHAR **src, WCHAR **dst, UINT len )
{
    str->Buffer = *dst;
    str->Length = len;
    str->MaximumLength = len + sizeof(WCHAR);
    memcpy( str->Buffer, *src, len );
    str->Buffer[len / sizeof(WCHAR)] = 0;
    *src += len / sizeof(WCHAR);
    *dst += len / sizeof(WCHAR) + 1;
}

/***********************************************************************
 *           init_user_process_params
 *
 * Fill the RTL_USER_PROCESS_PARAMETERS structure from the server.
 */
static NTSTATUS init_user_process_params( SIZE_T data_size )
{
    void *ptr;
    WCHAR *src, *dst;
    SIZE_T info_size, env_size, size, alloc_size;
    NTSTATUS status;
    startup_info_t *info;
    RTL_USER_PROCESS_PARAMETERS *params = NULL;

    if (!(info = RtlAllocateHeap( GetProcessHeap(), 0, data_size )))
        return STATUS_NO_MEMORY;

    SERVER_START_REQ( get_startup_info )
    {
        wine_server_set_reply( req, info, data_size );
        if (!(status = wine_server_call( req )))
        {
            data_size = wine_server_reply_size( reply );
            info_size = reply->info_size;
            env_size  = data_size - info_size;
        }
    }
    SERVER_END_REQ;
    if (status != STATUS_SUCCESS) goto done;

    size = sizeof(*params);
    size += sizeof(current_dir);
    size += info->dllpath_len + sizeof(WCHAR);
    size += info->imagepath_len + sizeof(WCHAR);
    size += info->cmdline_len + sizeof(WCHAR);
    size += info->title_len + sizeof(WCHAR);
    size += info->desktop_len + sizeof(WCHAR);
    size += info->shellinfo_len + sizeof(WCHAR);
    size += info->runtime_len + sizeof(WCHAR);

    alloc_size = size;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&params, 0, &alloc_size,
                                      MEM_COMMIT, PAGE_READWRITE );
    if (status != STATUS_SUCCESS) goto done;

    NtCurrentTeb()->Peb->ProcessParameters = params;
    params->AllocationSize  = alloc_size;
    params->Size            = size;
    params->Flags           = PROCESS_PARAMS_FLAG_NORMALIZED;
    params->DebugFlags      = info->debug_flags;
    params->ConsoleHandle   = wine_server_ptr_handle( info->console );
    params->ConsoleFlags    = info->console_flags;
    params->hStdInput       = wine_server_ptr_handle( info->hstdin );
    params->hStdOutput      = wine_server_ptr_handle( info->hstdout );
    params->hStdError       = wine_server_ptr_handle( info->hstderr );
    params->dwX             = info->x;
    params->dwY             = info->y;
    params->dwXSize         = info->xsize;
    params->dwYSize         = info->ysize;
    params->dwXCountChars   = info->xchars;
    params->dwYCountChars   = info->ychars;
    params->dwFillAttribute = info->attribute;
    params->dwFlags         = info->flags;
    params->wShowWindow     = info->show;

    src = (WCHAR *)(info + 1);
    dst = (WCHAR *)(params + 1);

    /* current directory needs more space */
    get_unicode_string( &params->CurrentDirectory.DosPath, &src, &dst, info->curdir_len );
    params->CurrentDirectory.DosPath.MaximumLength = sizeof(current_dir);
    dst = (WCHAR *)(params + 1) + ARRAY_SIZE(current_dir);

    get_unicode_string( &params->DllPath, &src, &dst, info->dllpath_len );
    get_unicode_string( &params->ImagePathName, &src, &dst, info->imagepath_len );
    get_unicode_string( &params->CommandLine, &src, &dst, info->cmdline_len );
    get_unicode_string( &params->WindowTitle, &src, &dst, info->title_len );
    get_unicode_string( &params->Desktop, &src, &dst, info->desktop_len );
    get_unicode_string( &params->ShellInfo, &src, &dst, info->shellinfo_len );

    /* runtime info isn't a real string */
    params->RuntimeInfo.Buffer = dst;
    params->RuntimeInfo.Length = params->RuntimeInfo.MaximumLength = info->runtime_len;
    memcpy( dst, src, info->runtime_len );

    /* environment needs to be a separate memory block */
    ptr = NULL;
    alloc_size = max( 1, env_size );
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &ptr, 0, &alloc_size,
                                      MEM_COMMIT, PAGE_READWRITE );
    if (status != STATUS_SUCCESS) goto done;
    memcpy( ptr, (char *)info + info_size, env_size );
    params->Environment = ptr;

done:
    RtlFreeHeap( GetProcessHeap(), 0, info );
    return status;
}

#ifdef __linux__

#ifdef HAVE_ELF_H
# include <elf.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#ifdef HAVE_SYS_AUXV_H
# include <sys/auxv.h>
#endif
#ifndef HAVE_GETAUXVAL
static unsigned long getauxval( unsigned long id )
{
    extern char **__wine_main_environ;
    char **ptr = __wine_main_environ;
    ElfW(auxv_t) *auxv;

    while (*ptr) ptr++;
    while (!*ptr) ptr++;
    for (auxv = (ElfW(auxv_t) *)ptr; auxv->a_type; auxv++)
        if (auxv->a_type == id) return auxv->a_un.a_val;
    return 0;
}
#endif

static ULONG_PTR get_image_addr(void)
{
    ULONG_PTR size, num, phdr_addr = getauxval( AT_PHDR );
    ElfW(Phdr) *phdr;

    if (!phdr_addr) return 0;
    phdr = (ElfW(Phdr) *)phdr_addr;
    size = getauxval( AT_PHENT );
    num = getauxval( AT_PHNUM );
    while (num--)
    {
        if (phdr->p_type == PT_PHDR) return phdr_addr - phdr->p_offset;
        phdr = (ElfW(Phdr) *)((char *)phdr + size);
    }
    return 0;
}

#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_error.h>

static ULONG_PTR get_image_addr(void)
{
    ULONG_PTR ret = 0;
#ifdef TASK_DYLD_INFO
    struct task_dyld_info dyld_info;
    mach_msg_type_number_t size = TASK_DYLD_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&dyld_info, &size) == KERN_SUCCESS)
        ret = dyld_info.all_image_info_addr;
#endif
    return ret;
}

#else
static ULONG_PTR get_image_addr(void)
{
    return 0;
}
#endif


/**************************************************************************
 *  __wine_user_shared_data   (NTDLL.@)
 *
 * Update user shared data and return the address of the structure.
 */
BYTE* CDECL __wine_user_shared_data(void)
{
    static int spinlock;
    ULARGE_INTEGER interrupt;
    LARGE_INTEGER now;
    
    while (interlocked_cmpxchg( &spinlock, 1, 0 ) != 0);
    
    NtQuerySystemTime( &now );
    user_shared_data->SystemTime.High2Time = now.u.HighPart;
    user_shared_data->SystemTime.LowPart   = now.u.LowPart;
    user_shared_data->SystemTime.High1Time = now.u.HighPart;

    RtlQueryUnbiasedInterruptTime( &interrupt.QuadPart );
    user_shared_data->InterruptTime.High2Time = interrupt.HighPart;
    user_shared_data->InterruptTime.LowPart   = interrupt.LowPart;
    user_shared_data->InterruptTime.High1Time = interrupt.HighPart;

    interrupt.QuadPart /= 10000;
    user_shared_data->u.TickCount.High2Time  = interrupt.HighPart;
    user_shared_data->u.TickCount.LowPart    = interrupt.LowPart;
    user_shared_data->u.TickCount.High1Time  = interrupt.HighPart;
    user_shared_data->TickCountLowDeprecated = interrupt.LowPart;
    user_shared_data->TickCountMultiplier = 1 << 24;
    
    spinlock = 0;
    return (BYTE *)user_shared_data;
}


static void *user_shared_data_thread(void *arg)
{
    struct timeval tv;

    while (TRUE)
    {
        __wine_user_shared_data();

        tv.tv_sec = 0;
        tv.tv_usec = 15600;
        select(0, NULL, NULL, NULL, &tv);
    }
    return NULL;
}


void create_user_shared_data_thread(void)
{
    static int thread_created;
    pthread_attr_t attr;
    pthread_t thread;

    if (interlocked_cmpxchg(&thread_created, 1, 0) != 0)
        return;

    FIXME("Creating user shared data update thread.\n");

    user_shared_data = user_shared_data_external;
    __wine_user_shared_data();

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);
    pthread_create(&thread, &attr, user_shared_data_thread, NULL);
    pthread_attr_destroy(&attr);
}


/***********************************************************************
 *           thread_init
 *
 * Setup the initial thread.
 *
 * NOTES: The first allocated TEB on NT is at 0x7ffde000.
 */
void thread_init(void)
{
    TEB *teb;
    void *addr;
    BOOL suspend;
    SIZE_T size, info_size;
    NTSTATUS status;
    struct ntdll_thread_data *thread_data;
    static struct debug_info debug_info;  /* debug info for initial thread */

    virtual_init();

    /* reserve space for shared user data */

    addr = (void *)0x7ffe0000;
    size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size,
                                      MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE );
    if (status)
    {
        MESSAGE( "wine: failed to map the shared user data: %08x\n", status );
        exit(1);
    }
    user_shared_data_external = addr;
    memcpy( user_shared_data->NtSystemRoot, default_windirW, sizeof(default_windirW) );

    /* allocate and initialize the PEB */

    addr = NULL;
    size = sizeof(*peb);
    NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 1, &size,
                             MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE );
    peb = addr;

    peb->FastPebLock        = &peb_lock;
    peb->ProcessParameters  = &params;
    peb->TlsBitmap          = &tls_bitmap;
    peb->TlsExpansionBitmap = &tls_expansion_bitmap;
    peb->FlsBitmap          = &fls_bitmap;
    peb->LdrData            = &ldr;
    peb->OSMajorVersion     = 5;
    peb->OSMinorVersion     = 1;
    peb->OSBuildNumber      = 0xA28;
    peb->OSPlatformId       = VER_PLATFORM_WIN32_NT;
    params.CurrentDirectory.DosPath.Buffer = current_dir;
    params.CurrentDirectory.DosPath.MaximumLength = sizeof(current_dir);
    params.wShowWindow = 1; /* SW_SHOWNORMAL */
    ldr.Length = sizeof(ldr);
    ldr.Initialized = TRUE;
    RtlInitializeBitMap( &tls_bitmap, peb->TlsBitmapBits, sizeof(peb->TlsBitmapBits) * 8 );
    RtlInitializeBitMap( &tls_expansion_bitmap, peb->TlsExpansionBitmapBits,
                         sizeof(peb->TlsExpansionBitmapBits) * 8 );
    RtlInitializeBitMap( &fls_bitmap, peb->FlsBitmapBits, sizeof(peb->FlsBitmapBits) * 8 );
    RtlSetBits( peb->TlsBitmap, 0, 1 ); /* TLS index 0 is reserved and should be initialized to NULL. */
    RtlSetBits( peb->FlsBitmap, 0, 1 );
    InitializeListHead( &peb->FlsListHead );
    InitializeListHead( &ldr.InLoadOrderModuleList );
    InitializeListHead( &ldr.InMemoryOrderModuleList );
    InitializeListHead( &ldr.InInitializationOrderModuleList );
    *(ULONG_PTR *)peb->Reserved = get_image_addr();

#if defined(__APPLE__) && defined(__x86_64__)
    *((DWORD*)((char*)user_shared_data_external + 0x1000)) = __wine_syscall_dispatcher;
#endif

    /*
     * Starting with Vista, the first user to log on has session id 1.
     * Session id 0 is for processes that don't interact with the user (like services).
     */
    peb->SessionId = 1;

    /* allocate and initialize the initial TEB */

    signal_alloc_thread( &teb );
    teb->Peb = peb;
    teb->Tib.StackBase = (void *)~0UL;
    teb->StaticUnicodeString.Buffer = teb->StaticUnicodeBuffer;
    teb->StaticUnicodeString.MaximumLength = sizeof(teb->StaticUnicodeBuffer);
    teb->ThreadLocalStoragePointer = teb->TlsSlots;

    thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    thread_data->request_fd = -1;
    thread_data->reply_fd   = -1;
    thread_data->wait_fd[0] = -1;
    thread_data->wait_fd[1] = -1;
    thread_data->debug_info = &debug_info;

    signal_init_thread( teb );
    virtual_init_threading();

    debug_info.str_pos = debug_info.strings;
    debug_info.out_pos = debug_info.output;
    debug_init();

    /* setup the server connection */
    server_init_process();
    info_size = server_init_thread( peb, &suspend );

    /* create the process heap */
    if (!(peb->ProcessHeap = RtlCreateHeap( HEAP_GROWABLE, NULL, 0, 0, NULL, NULL )))
    {
        MESSAGE( "wine: failed to create the process heap\n" );
        exit(1);
    }

    /* allocate user parameters */
    if (info_size)
    {
        init_user_process_params( info_size );
    }
    else
    {
        if (isatty(0) || isatty(1) || isatty(2))
            params.ConsoleHandle = (HANDLE)2; /* see kernel32/kernel_private.h */
        if (!isatty(0))
            wine_server_fd_to_handle( 0, GENERIC_READ|SYNCHRONIZE,  OBJ_INHERIT, &params.hStdInput );
        if (!isatty(1))
            wine_server_fd_to_handle( 1, GENERIC_WRITE|SYNCHRONIZE, OBJ_INHERIT, &params.hStdOutput );
        if (!isatty(2))
            wine_server_fd_to_handle( 2, GENERIC_WRITE|SYNCHRONIZE, OBJ_INHERIT, &params.hStdError );
    }

    /* initialize user_shared_data */
    __wine_user_shared_data();
    fill_cpu_info();

    NtCreateKeyedEvent( &keyed_event, GENERIC_READ | GENERIC_WRITE, NULL, 0 );
}


/***********************************************************************
 *           free_thread_data
 */
static void free_thread_data( TEB *teb )
{
    struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    SIZE_T size;

    if (teb->DeallocationStack)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &teb->DeallocationStack, &size, MEM_RELEASE );
    }
    if (thread_data->start_stack)
    {
        size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &thread_data->start_stack, &size, MEM_RELEASE );
    }
    signal_free_thread( teb );
}


/***********************************************************************
 *           abort_thread
 */
void abort_thread( int status )
{
    pthread_sigmask( SIG_BLOCK, &server_block_set, NULL );
    if (interlocked_xchg_add( &nb_threads, -1 ) <= 1) _exit( status );
    signal_exit_thread( status );
}


/***********************************************************************
 *           exit_thread
 */
void exit_thread( int status )
{
    close( ntdll_get_thread_data()->wait_fd[0] );
    close( ntdll_get_thread_data()->wait_fd[1] );
    close( ntdll_get_thread_data()->reply_fd );
    close( ntdll_get_thread_data()->request_fd );
    pthread_exit( UIntToPtr(status) );
}


/***********************************************************************
 *           RtlExitUserThread  (NTDLL.@)
 */
void WINAPI RtlExitUserThread( ULONG status )
{
    static void *prev_teb;
    TEB *teb;

    if (status)  /* send the exit code to the server (0 is already the default) */
    {
        SERVER_START_REQ( terminate_thread )
        {
            req->handle    = wine_server_obj_handle( GetCurrentThread() );
            req->exit_code = status;
            wine_server_call( req );
        }
        SERVER_END_REQ;
    }

    if (interlocked_xchg_add( &nb_threads, -1 ) <= 1)
    {
        LdrShutdownProcess();
        pthread_sigmask( SIG_BLOCK, &server_block_set, NULL );
        signal_exit_process( status );
    }

    LdrShutdownThread();
    RtlFreeThreadActivationContextStack();

    pthread_sigmask( SIG_BLOCK, &server_block_set, NULL );

    if ((teb = interlocked_xchg_ptr( &prev_teb, NtCurrentTeb() )))
    {
        struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;

        if (thread_data->pthread_id)
        {
            pthread_join( thread_data->pthread_id, NULL );
            free_thread_data( teb );
        }
    }

    signal_exit_thread( status );
}


/***********************************************************************
 *           start_thread
 *
 * Startup routine for a newly created thread.
 */
static void start_thread( struct startup_info *info )
{
    BOOL suspend;
    TEB *teb = info->teb;
    struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    struct debug_info debug_info;

    debug_info.str_pos = debug_info.strings;
    debug_info.out_pos = debug_info.output;
    thread_data->debug_info = &debug_info;
    thread_data->pthread_id = pthread_self();

    signal_init_thread( teb );
    server_init_thread( info->entry_point, &suspend );
    signal_start_thread( info->entry_point, info->entry_arg, suspend );
}


/***********************************************************************
 *              NtCreateThreadEx   (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateThreadEx( HANDLE *handle_ptr, ACCESS_MASK access, OBJECT_ATTRIBUTES *thread_attr,
                                  HANDLE process, LPTHREAD_START_ROUTINE start, void *param,
                                  ULONG flags, ULONG zero_bits, ULONG stack_commit,
                                  ULONG stack_reserve, PPS_ATTRIBUTE_LIST ps_attr_list )
{
    sigset_t sigset;
    pthread_t pthread_id;
    pthread_attr_t pthread_attr;
    struct ntdll_thread_data *thread_data;
    struct startup_info *info;
    BOOLEAN suspended = !!(flags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED);
    CLIENT_ID *id = NULL;
    HANDLE handle = 0, actctx = 0;
    TEB *teb = NULL;
    DWORD tid = 0;
    int request_pipe[2];
    NTSTATUS status;
    SIZE_T extra_stack = PTHREAD_STACK_MIN;
    data_size_t len = 0;
    struct object_attributes *objattr = NULL;

    TRACE("(%p, %d, %p, %p, %p, %p, %u, %u, %u, %u, %p)\n",
          handle_ptr, access, thread_attr, process, start, param, flags,
          zero_bits, stack_commit, stack_reserve, ps_attr_list);

    if (ps_attr_list != NULL)
    {
        PS_ATTRIBUTE *ps_attr,
                     *ps_attr_end = (PS_ATTRIBUTE *)((UINT_PTR)ps_attr_list + ps_attr_list->TotalLength);
        for (ps_attr = &ps_attr_list->Attributes[0]; ps_attr < ps_attr_end; ps_attr++)
        {
            switch (ps_attr->Attribute)
            {
            case PS_ATTRIBUTE_CLIENT_ID:
                /* TODO validate ps_attr->Size == sizeof(CLIENT_ID) */
                /* TODO set *ps_attr->ReturnLength */
                id = ps_attr->ValuePtr;
                break;
            default:
                FIXME("Unsupported attribute %08X\n", ps_attr->Attribute);
                break;
            }
        }
    }

    if (access == (ACCESS_MASK)0)
        access = THREAD_ALL_ACCESS;

    if (process != NtCurrentProcess())
    {
        apc_call_t call;
        apc_result_t result;

        memset( &call, 0, sizeof(call) );

        call.create_thread.type    = APC_CREATE_THREAD;
        call.create_thread.func    = wine_server_client_ptr( start );
        call.create_thread.arg     = wine_server_client_ptr( param );
        call.create_thread.reserve = stack_reserve;
        call.create_thread.commit  = stack_commit;
        call.create_thread.suspend = suspended;
        status = server_queue_process_apc( process, &call, &result );
        if (status != STATUS_SUCCESS) return status;

        if (result.create_thread.status == STATUS_SUCCESS)
        {
            if (id) id->UniqueThread = ULongToHandle(result.create_thread.tid);
            if (handle_ptr) *handle_ptr = wine_server_ptr_handle( result.create_thread.handle );
            else NtClose( wine_server_ptr_handle( result.create_thread.handle ));
        }
        return result.create_thread.status;
    }

    if ((status = alloc_object_attributes( thread_attr, &objattr, &len ))) return status;

    if (server_pipe( request_pipe ) == -1)
    {
        RtlFreeHeap( GetProcessHeap(), 0, objattr );
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    wine_server_send_fd( request_pipe[0] );

    SERVER_START_REQ( new_thread )
    {
        req->process    = wine_server_obj_handle( process );
        req->access     = access;
        req->suspend    = suspended;
        req->request_fd = request_pipe[0];
        wine_server_add_data( req, objattr, len );
        if (!(status = wine_server_call( req )))
        {
            handle = wine_server_ptr_handle( reply->handle );
            tid = reply->tid;
        }
        close( request_pipe[0] );
    }
    SERVER_END_REQ;

    RtlFreeHeap( GetProcessHeap(), 0, objattr );
    if (status)
    {
        close( request_pipe[1] );
        return status;
    }

    pthread_sigmask( SIG_BLOCK, &server_block_set, &sigset );

    if ((status = signal_alloc_thread( &teb ))) goto error;

    teb->Peb = NtCurrentTeb()->Peb;
    teb->ClientId.UniqueProcess = ULongToHandle(GetCurrentProcessId());
    teb->ClientId.UniqueThread  = ULongToHandle(tid);
    teb->StaticUnicodeString.Buffer        = teb->StaticUnicodeBuffer;
    teb->StaticUnicodeString.MaximumLength = sizeof(teb->StaticUnicodeBuffer);
    teb->ThreadLocalStoragePointer = teb->TlsSlots;

    /* create default activation context frame for new thread */
    RtlGetActiveActivationContext(&actctx);
    if (actctx)
    {
        RTL_ACTIVATION_CONTEXT_STACK_FRAME *frame;

        frame = RtlAllocateHeap(GetProcessHeap(), 0, sizeof(*frame));
        frame->Previous = NULL;
        frame->ActivationContext = actctx;
        frame->Flags = 0;
        teb->ActivationContextStack.ActiveFrame = frame;
    }

    info = (struct startup_info *)(teb + 1);
    info->teb         = teb;
    info->entry_point = start;
    info->entry_arg   = param;

    if ((status = virtual_alloc_thread_stack( teb, stack_reserve, stack_commit, &extra_stack )))
        goto error;

    thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    thread_data->request_fd  = request_pipe[1];
    thread_data->reply_fd    = -1;
    thread_data->wait_fd[0]  = -1;
    thread_data->wait_fd[1]  = -1;
    thread_data->start_stack = (char *)teb->Tib.StackBase;

    pthread_attr_init( &pthread_attr );
    pthread_attr_setstack( &pthread_attr, teb->DeallocationStack,
                         (char *)teb->Tib.StackBase + extra_stack - (char *)teb->DeallocationStack );
    pthread_attr_setscope( &pthread_attr, PTHREAD_SCOPE_SYSTEM ); /* force creating a kernel thread */
    interlocked_xchg_add( &nb_threads, 1 );
    if (pthread_create( &pthread_id, &pthread_attr, (void * (*)(void *))start_thread, info ))
    {
        interlocked_xchg_add( &nb_threads, -1 );
        pthread_attr_destroy( &pthread_attr );
        status = STATUS_NO_MEMORY;
        goto error;
    }
    pthread_attr_destroy( &pthread_attr );
    pthread_sigmask( SIG_SETMASK, &sigset, NULL );

    if (id) id->UniqueThread = ULongToHandle(tid);
    if (handle_ptr) *handle_ptr = handle;
    else NtClose( handle );

    return STATUS_SUCCESS;

error:
    if (teb) free_thread_data( teb );
    if (handle) NtClose( handle );
    pthread_sigmask( SIG_SETMASK, &sigset, NULL );
    close( request_pipe[1] );
    return status;
}

NTSTATUS WINAPI NtCreateThread( HANDLE *handle_ptr, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr, HANDLE process,
                                CLIENT_ID *id, CONTEXT *context, INITIAL_TEB *teb, BOOLEAN suspended )
{
    LPTHREAD_START_ROUTINE entry;
    void *arg;
    ULONG flags = suspended ? THREAD_CREATE_FLAGS_CREATE_SUSPENDED : 0;
    PS_ATTRIBUTE_LIST attr_list, *pattr_list = NULL;

#if defined(__i386__)
        entry = (LPTHREAD_START_ROUTINE) context->Eax;
        arg = (void *)context->Ebx;
#elif defined(__x86_64__)
        entry = (LPTHREAD_START_ROUTINE) context->Rcx;
        arg = (void *)context->Rdx;
#elif defined(__arm__)
        entry = (LPTHREAD_START_ROUTINE) context->R0;
        arg = (void *)context->R1;
#elif defined(__aarch64__)
        entry = (LPTHREAD_START_ROUTINE) context->u.X0;
        arg = (void *)context->u.X1;
#elif defined(__powerpc__)
        entry = (LPTHREAD_START_ROUTINE) context->Gpr3;
        arg = (void *)context->Gpr4;
#endif

    if (id)
    {
        attr_list.TotalLength = sizeof(PS_ATTRIBUTE_LIST);
        attr_list.Attributes[0].Attribute = PS_ATTRIBUTE_CLIENT_ID;
        attr_list.Attributes[0].Size = sizeof(CLIENT_ID);
        attr_list.Attributes[0].ValuePtr = id;
        attr_list.Attributes[0].ReturnLength = NULL;
        pattr_list = &attr_list;
    }

    return NtCreateThreadEx(handle_ptr, access, attr, process, entry, arg, flags, 0, 0, 0, pattr_list);
}

NTSTATUS WINAPI __syscall_NtCreateThread( HANDLE *handle_ptr, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr,
                                          HANDLE process, CLIENT_ID *id, CONTEXT *context, INITIAL_TEB *teb,
                                          BOOLEAN suspended );
NTSTATUS WINAPI __syscall_NtCreateThreadEx( HANDLE *handle_ptr, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr,
                                            HANDLE process, LPTHREAD_START_ROUTINE start, void *param,
                                            ULONG flags, ULONG zero_bits, ULONG stack_commit,
                                            ULONG stack_reserve, PPS_ATTRIBUTE_LIST ps_attr_list );

/***********************************************************************
 *              RtlCreateUserThread   (NTDLL.@)
 */
NTSTATUS WINAPI RtlCreateUserThread( HANDLE process, SECURITY_DESCRIPTOR *descr,
                                     BOOLEAN suspended, void *stack_addr,
                                     SIZE_T stack_reserve, SIZE_T stack_commit,
                                     PRTL_THREAD_START_ROUTINE entry, void *arg,
                                     HANDLE *handle_ptr, CLIENT_ID *id )
{
    OBJECT_ATTRIBUTES thread_attr;
    InitializeObjectAttributes( &thread_attr, NULL, 0, NULL, descr );
    if (stack_addr)
        FIXME("stack_addr != NULL is unimplemented\n");

    if (NtCurrentTeb()->Peb->OSMajorVersion < 6)
    {
        /* Use old API. */
        CONTEXT context = { 0 };

        if (stack_commit)
            FIXME("stack_commit != 0 is unimplemented\n");
        if (stack_reserve)
            FIXME("stack_reserve != 0 is unimplemented\n");

        context.ContextFlags = CONTEXT_FULL;
#if defined(__i386__)
        context.Eax = (DWORD)entry;
        context.Ebx = (DWORD)arg;
#elif defined(__x86_64__)
        context.Rcx = (ULONG_PTR)entry;
        context.Rdx = (ULONG_PTR)arg;
#elif defined(__arm__)
        context.R0 = (DWORD)entry;
        context.R1 = (DWORD)arg;
#elif defined(__aarch64__)
        context.u.X0 = (DWORD_PTR)entry;
        context.u.X1 = (DWORD_PTR)arg;
#elif defined(__powerpc__)
        context.Gpr3 = (DWORD)entry;
        context.Gpr4 = (DWORD)arg;
#endif

#if defined(__i386__) || defined(__x86_64__)
        return __syscall_NtCreateThread(handle_ptr, (ACCESS_MASK)0, &thread_attr, process, id, &context, NULL, suspended);
#else
        return NtCreateThread(handle_ptr, (ACCESS_MASK)0, &thread_attr, process, id, &context, NULL, suspended);
#endif
    }
    else
    {
        /* Use new API from Vista+. */
        ULONG flags = suspended ? THREAD_CREATE_FLAGS_CREATE_SUSPENDED : 0;
        PS_ATTRIBUTE_LIST attr_list, *pattr_list = NULL;

        if (id)
        {
            attr_list.TotalLength = sizeof(PS_ATTRIBUTE_LIST);
            attr_list.Attributes[0].Attribute = PS_ATTRIBUTE_CLIENT_ID;
            attr_list.Attributes[0].Size = sizeof(CLIENT_ID);
            attr_list.Attributes[0].ValuePtr = id;
            attr_list.Attributes[0].ReturnLength = NULL;
            pattr_list = &attr_list;
        }

#if defined(__i386__) || defined(__x86_64__)
        return __syscall_NtCreateThreadEx(handle_ptr, (ACCESS_MASK)0, &thread_attr, process, (LPTHREAD_START_ROUTINE)entry, arg, flags, 0, stack_commit, stack_reserve, pattr_list);
#else
        return NtCreateThreadEx(handle_ptr, (ACCESS_MASK)0, &thread_attr, process, (LPTHREAD_START_ROUTINE)entry, arg, flags, 0, stack_commit, stack_reserve, pattr_list);
#endif
    }
}


/******************************************************************************
 *              RtlGetNtGlobalFlags   (NTDLL.@)
 */
ULONG WINAPI RtlGetNtGlobalFlags(void)
{
    if (!peb) return 0;  /* init not done yet */
    return peb->NtGlobalFlag;
}


/***********************************************************************
 *              NtOpenThread   (NTDLL.@)
 *              ZwOpenThread   (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenThread( HANDLE *handle, ACCESS_MASK access,
                              const OBJECT_ATTRIBUTES *attr, const CLIENT_ID *id )
{
    NTSTATUS ret;

    SERVER_START_REQ( open_thread )
    {
        req->tid        = HandleToULong(id->UniqueThread);
        req->access     = access;
        req->attributes = attr ? attr->Attributes : 0;
        ret = wine_server_call( req );
        *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtSuspendThread   (NTDLL.@)
 *              ZwSuspendThread   (NTDLL.@)
 */
NTSTATUS WINAPI NtSuspendThread( HANDLE handle, PULONG count )
{
    NTSTATUS ret;

    SERVER_START_REQ( suspend_thread )
    {
        req->handle = wine_server_obj_handle( handle );
        if (!(ret = wine_server_call( req )))
        {
            if (count) *count = reply->count;
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtResumeThread   (NTDLL.@)
 *              ZwResumeThread   (NTDLL.@)
 */
NTSTATUS WINAPI NtResumeThread( HANDLE handle, PULONG count )
{
    NTSTATUS ret;

    SERVER_START_REQ( resume_thread )
    {
        req->handle = wine_server_obj_handle( handle );
        if (!(ret = wine_server_call( req )))
        {
            if (count) *count = reply->count;
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtAlertResumeThread   (NTDLL.@)
 *              ZwAlertResumeThread   (NTDLL.@)
 */
NTSTATUS WINAPI NtAlertResumeThread( HANDLE handle, PULONG count )
{
    FIXME( "stub: should alert thread %p\n", handle );
    return NtResumeThread( handle, count );
}


/******************************************************************************
 *              NtAlertThread   (NTDLL.@)
 *              ZwAlertThread   (NTDLL.@)
 */
NTSTATUS WINAPI NtAlertThread( HANDLE handle )
{
    FIXME( "stub: %p\n", handle );
    return STATUS_NOT_IMPLEMENTED;
}


/******************************************************************************
 *              NtTerminateThread  (NTDLL.@)
 *              ZwTerminateThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtTerminateThread( HANDLE handle, LONG exit_code )
{
    NTSTATUS ret;
    BOOL self;

    SERVER_START_REQ( terminate_thread )
    {
        req->handle    = wine_server_obj_handle( handle );
        req->exit_code = exit_code;
        ret = wine_server_call( req );
        self = !ret && reply->self;
    }
    SERVER_END_REQ;

    if (self) abort_thread( exit_code );
    return ret;
}


/******************************************************************************
 *              NtQueueApcThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtQueueApcThread( HANDLE handle, PNTAPCFUNC func, ULONG_PTR arg1,
                                  ULONG_PTR arg2, ULONG_PTR arg3 )
{
    NTSTATUS ret;
    SERVER_START_REQ( queue_apc )
    {
        req->handle = wine_server_obj_handle( handle );
        if (func)
        {
            req->call.type         = APC_USER;
            req->call.user.func    = wine_server_client_ptr( func );
            req->call.user.args[0] = arg1;
            req->call.user.args[1] = arg2;
            req->call.user.args[2] = arg3;
        }
        else req->call.type = APC_NONE;  /* wake up only */
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              RtlPushFrame  (NTDLL.@)
 */
void WINAPI RtlPushFrame( TEB_ACTIVE_FRAME *frame )
{
    frame->Previous = NtCurrentTeb()->ActiveFrame;
    NtCurrentTeb()->ActiveFrame = frame;
}


/******************************************************************************
 *              RtlPopFrame  (NTDLL.@)
 */
void WINAPI RtlPopFrame( TEB_ACTIVE_FRAME *frame )
{
    NtCurrentTeb()->ActiveFrame = frame->Previous;
}


/******************************************************************************
 *              RtlGetFrame  (NTDLL.@)
 */
TEB_ACTIVE_FRAME * WINAPI RtlGetFrame(void)
{
    return NtCurrentTeb()->ActiveFrame;
}


/***********************************************************************
 *              set_thread_context
 */
NTSTATUS set_thread_context( HANDLE handle, const context_t *context, BOOL *self )
{
    NTSTATUS ret;
    DWORD dummy, i;

    SERVER_START_REQ( set_thread_context )
    {
        req->handle  = wine_server_obj_handle( handle );
        req->suspend = 1;
        wine_server_add_data( req, context, sizeof(*context) );
        ret = wine_server_call( req );
        *self = reply->self;
    }
    SERVER_END_REQ;

    if (ret == STATUS_PENDING)
    {
        for (i = 0; i < 100; i++)
        {
            SERVER_START_REQ( set_thread_context )
            {
                req->handle  = wine_server_obj_handle( handle );
                req->suspend = 0;
                wine_server_add_data( req, context, sizeof(*context) );
                ret = wine_server_call( req );
            }
            SERVER_END_REQ;
            if (ret == STATUS_PENDING)
            {
                LARGE_INTEGER timeout;
                timeout.QuadPart = -10000;
                NtDelayExecution( FALSE, &timeout );
            }
            else break;
        }
        NtResumeThread( handle, &dummy );
        if (ret == STATUS_PENDING) ret = STATUS_ACCESS_DENIED;
    }

    return ret;
}


/***********************************************************************
 *              get_thread_context
 */
NTSTATUS get_thread_context( HANDLE handle, context_t *context, unsigned int flags, BOOL *self )
{
    NTSTATUS ret;
    DWORD dummy, i;

    SERVER_START_REQ( get_thread_context )
    {
        req->handle  = wine_server_obj_handle( handle );
        req->flags   = flags;
        req->suspend = 1;
        wine_server_set_reply( req, context, sizeof(*context) );
        ret = wine_server_call( req );
        *self = reply->self;
    }
    SERVER_END_REQ;

    if (ret == STATUS_PENDING)
    {
        for (i = 0; i < 100; i++)
        {
            SERVER_START_REQ( get_thread_context )
            {
                req->handle  = wine_server_obj_handle( handle );
                req->flags   = flags;
                req->suspend = 0;
                wine_server_set_reply( req, context, sizeof(*context) );
                ret = wine_server_call( req );
            }
            SERVER_END_REQ;
            if (ret == STATUS_PENDING)
            {
                LARGE_INTEGER timeout;
                timeout.QuadPart = -10000;
                NtDelayExecution( FALSE, &timeout );
            }
            else break;
        }
        NtResumeThread( handle, &dummy );
        if (ret == STATUS_PENDING) ret = STATUS_ACCESS_DENIED;
    }
    return ret;
}


/******************************************************************************
 *              NtQueryInformationThread  (NTDLL.@)
 *              ZwQueryInformationThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryInformationThread( HANDLE handle, THREADINFOCLASS class,
                                          void *data, ULONG length, ULONG *ret_len )
{
    NTSTATUS status;

    switch(class)
    {
    case ThreadBasicInformation:
        {
            THREAD_BASIC_INFORMATION info;
            const ULONG_PTR affinity_mask = get_system_affinity_mask();

            SERVER_START_REQ( get_thread_info )
            {
                req->handle = wine_server_obj_handle( handle );
                req->tid_in = 0;
                if (!(status = wine_server_call( req )))
                {
                    info.ExitStatus             = reply->exit_code;
                    info.TebBaseAddress         = wine_server_get_ptr( reply->teb );
                    info.ClientId.UniqueProcess = ULongToHandle(reply->pid);
                    info.ClientId.UniqueThread  = ULongToHandle(reply->tid);
                    info.AffinityMask           = reply->affinity & affinity_mask;
                    info.Priority               = reply->priority;
                    info.BasePriority           = reply->priority;  /* FIXME */
                }
            }
            SERVER_END_REQ;
            if (status == STATUS_SUCCESS)
            {
                if (data) memcpy( data, &info, min( length, sizeof(info) ));
                if (ret_len) *ret_len = min( length, sizeof(info) );
            }
        }
        return status;
    case ThreadAffinityMask:
        {
            const ULONG_PTR affinity_mask = get_system_affinity_mask();
            ULONG_PTR affinity = 0;

            SERVER_START_REQ( get_thread_info )
            {
                req->handle = wine_server_obj_handle( handle );
                req->tid_in = 0;
                if (!(status = wine_server_call( req )))
                    affinity = reply->affinity & affinity_mask;
            }
            SERVER_END_REQ;
            if (status == STATUS_SUCCESS)
            {
                if (data) memcpy( data, &affinity, min( length, sizeof(affinity) ));
                if (ret_len) *ret_len = min( length, sizeof(affinity) );
            }
        }
        return status;
    case ThreadTimes:
        {
            KERNEL_USER_TIMES   kusrt;

            SERVER_START_REQ( get_thread_times )
            {
                req->handle = wine_server_obj_handle( handle );
                status = wine_server_call( req );
                if (status == STATUS_SUCCESS)
                {
                    kusrt.CreateTime.QuadPart = reply->creation_time;
                    kusrt.ExitTime.QuadPart = reply->exit_time;
                }
            }
            SERVER_END_REQ;
            if (status == STATUS_SUCCESS)
            {
                /* We call times(2) for kernel time or user time */
                /* We can only (portably) do this for the current thread */
                if (handle == GetCurrentThread())
                {
                    struct tms time_buf;
                    long clocks_per_sec = sysconf(_SC_CLK_TCK);

                    times(&time_buf);
                    kusrt.KernelTime.QuadPart = (ULONGLONG)time_buf.tms_stime * 10000000 / clocks_per_sec;
                    kusrt.UserTime.QuadPart = (ULONGLONG)time_buf.tms_utime * 10000000 / clocks_per_sec;
                }
                else
                {
                    static BOOL reported = FALSE;

                    kusrt.KernelTime.QuadPart = 0;
                    kusrt.UserTime.QuadPart = 0;
                    if (reported)
                        TRACE("Cannot get kerneltime or usertime of other threads\n");
                    else
                    {
                        FIXME("Cannot get kerneltime or usertime of other threads\n");
                        reported = TRUE;
                    }
                }
                if (data) memcpy( data, &kusrt, min( length, sizeof(kusrt) ));
                if (ret_len) *ret_len = min( length, sizeof(kusrt) );
            }
        }
        return status;
    case ThreadDescriptorTableEntry:
        {
#ifdef __i386__
            THREAD_DESCRIPTOR_INFORMATION*      tdi = data;
            if (length < sizeof(*tdi))
                status = STATUS_INFO_LENGTH_MISMATCH;
            else if (!(tdi->Selector & 4))  /* GDT selector */
            {
                unsigned sel = LOWORD(tdi->Selector) & ~3;  /* ignore RPL */
                status = STATUS_SUCCESS;
                if (!sel)  /* null selector */
                    memset( &tdi->Entry, 0, sizeof(tdi->Entry) );
                else
                {
                    tdi->Entry.BaseLow                   = 0;
                    tdi->Entry.HighWord.Bits.BaseMid     = 0;
                    tdi->Entry.HighWord.Bits.BaseHi      = 0;
                    tdi->Entry.LimitLow                  = 0xffff;
                    tdi->Entry.HighWord.Bits.LimitHi     = 0xf;
                    tdi->Entry.HighWord.Bits.Dpl         = 3;
                    tdi->Entry.HighWord.Bits.Sys         = 0;
                    tdi->Entry.HighWord.Bits.Pres        = 1;
                    tdi->Entry.HighWord.Bits.Granularity = 1;
                    tdi->Entry.HighWord.Bits.Default_Big = 1;
                    tdi->Entry.HighWord.Bits.Type        = 0x12;
                    tdi->Entry.HighWord.Bits.Reserved_0  = 0;
                    /* it has to be one of the system GDT selectors */
                    if (sel != (wine_get_ds() & ~3) && sel != (wine_get_ss() & ~3))
                    {
                        if (sel == (wine_get_cs() & ~3))
                            tdi->Entry.HighWord.Bits.Type |= 8;  /* code segment */
                        else if (sel == (wine_get_fs() & ~3))
                        {
                            ULONG_PTR fs_base = (ULONG_PTR)NtCurrentTeb();
                            tdi->Entry.BaseLow                   = fs_base & 0xffff;
                            tdi->Entry.HighWord.Bits.BaseMid     = (fs_base >> 16) & 0xff;
                            tdi->Entry.HighWord.Bits.BaseHi      = (fs_base >> 24) & 0xff;
                            tdi->Entry.LimitLow                  = 0x0fff;
                            tdi->Entry.HighWord.Bits.LimitHi     = 0;
                            tdi->Entry.HighWord.Bits.Granularity = 0;
                        }
                        else status = STATUS_ACCESS_DENIED;
                    }
                }
            }
            else
            {
                SERVER_START_REQ( get_selector_entry )
                {
                    req->handle = wine_server_obj_handle( handle );
                    req->entry = LOWORD(tdi->Selector) >> 3;
                    status = wine_server_call( req );
                    if (!status)
                    {
                        if (!(reply->flags & WINE_LDT_FLAGS_ALLOCATED))
                            status = STATUS_ACCESS_VIOLATION;
                        else
                        {
                            wine_ldt_set_base ( &tdi->Entry, (void *)reply->base );
                            wine_ldt_set_limit( &tdi->Entry, reply->limit );
                            wine_ldt_set_flags( &tdi->Entry, reply->flags );
                        }
                    }
                }
                SERVER_END_REQ;
            }
            if (status == STATUS_SUCCESS && ret_len)
                /* yes, that's a bit strange, but it's the way it is */
                *ret_len = sizeof(LDT_ENTRY);
#else
            status = STATUS_NOT_IMPLEMENTED;
#endif
            return status;
        }
    case ThreadAmILastThread:
        {
            SERVER_START_REQ(get_thread_info)
            {
                req->handle = wine_server_obj_handle( handle );
                req->tid_in = 0;
                status = wine_server_call( req );
                if (status == STATUS_SUCCESS)
                {
                    BOOLEAN last = reply->last;
                    if (data) memcpy( data, &last, min( length, sizeof(last) ));
                    if (ret_len) *ret_len = min( length, sizeof(last) );
                }
            }
            SERVER_END_REQ;
            return status;
        }
    case ThreadQuerySetWin32StartAddress:
        {
            SERVER_START_REQ( get_thread_info )
            {
                req->handle = wine_server_obj_handle( handle );
                req->tid_in = 0;
                status = wine_server_call( req );
                if (status == STATUS_SUCCESS)
                {
                    PRTL_THREAD_START_ROUTINE entry = wine_server_get_ptr( reply->entry_point );
                    if (data) memcpy( data, &entry, min( length, sizeof(entry) ) );
                    if (ret_len) *ret_len = min( length, sizeof(entry) );
                }
            }
            SERVER_END_REQ;
            return status;
        }
    case ThreadGroupInformation:
        {
            const ULONG_PTR affinity_mask = get_system_affinity_mask();
            GROUP_AFFINITY affinity;

            memset(&affinity, 0, sizeof(affinity));
            affinity.Group = 0; /* Wine only supports max 64 processors */

            SERVER_START_REQ( get_thread_info )
            {
                req->handle = wine_server_obj_handle( handle );
                req->tid_in = 0;
                if (!(status = wine_server_call( req )))
                    affinity.Mask = reply->affinity & affinity_mask;
            }
            SERVER_END_REQ;
            if (status == STATUS_SUCCESS)
            {
                if (data) memcpy( data, &affinity, min( length, sizeof(affinity) ));
                if (ret_len) *ret_len = min( length, sizeof(affinity) );
            }
        }
        return status;
    case ThreadIsIoPending:
        FIXME( "ThreadIsIoPending info class not supported yet\n" );
        if (length != sizeof(BOOL)) return STATUS_INFO_LENGTH_MISMATCH;
        if (!data) return STATUS_ACCESS_DENIED;

        *(BOOL*)data = FALSE;
        if (ret_len) *ret_len = sizeof(BOOL);
        return STATUS_SUCCESS;
    case ThreadPriority:
    case ThreadBasePriority:
    case ThreadImpersonationToken:
    case ThreadEnableAlignmentFaultFixup:
    case ThreadEventPair_Reusable:
    case ThreadZeroTlsCell:
    case ThreadPerformanceCount:
    case ThreadIdealProcessor:
    case ThreadPriorityBoost:
    case ThreadSetTlsArrayAddress:
    default:
        FIXME( "info class %d not supported yet\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}


/******************************************************************************
 *              NtSetInformationThread  (NTDLL.@)
 *              ZwSetInformationThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetInformationThread( HANDLE handle, THREADINFOCLASS class,
                                        LPCVOID data, ULONG length )
{
    NTSTATUS status;
    switch(class)
    {
    case ThreadZeroTlsCell:
        if (handle == GetCurrentThread())
        {
            LIST_ENTRY *entry;
            DWORD index;

            if (length != sizeof(DWORD)) return STATUS_INVALID_PARAMETER;
            index = *(const DWORD *)data;
            if (index < TLS_MINIMUM_AVAILABLE)
            {
                RtlAcquirePebLock();
                for (entry = tls_links.Flink; entry != &tls_links; entry = entry->Flink)
                {
                    TEB *teb = CONTAINING_RECORD(entry, TEB, TlsLinks);
                    teb->TlsSlots[index] = 0;
                }
                RtlReleasePebLock();
            }
            else
            {
                index -= TLS_MINIMUM_AVAILABLE;
                if (index >= 8 * sizeof(NtCurrentTeb()->Peb->TlsExpansionBitmapBits))
                    return STATUS_INVALID_PARAMETER;
                RtlAcquirePebLock();
                for (entry = tls_links.Flink; entry != &tls_links; entry = entry->Flink)
                {
                    TEB *teb = CONTAINING_RECORD(entry, TEB, TlsLinks);
                    if (teb->TlsExpansionSlots) teb->TlsExpansionSlots[index] = 0;
                }
                RtlReleasePebLock();
            }
            return STATUS_SUCCESS;
        }
        FIXME( "ZeroTlsCell not supported on other threads\n" );
        return STATUS_NOT_IMPLEMENTED;

    case ThreadImpersonationToken:
        {
            const HANDLE *phToken = data;
            if (length != sizeof(HANDLE)) return STATUS_INVALID_PARAMETER;
            TRACE("Setting ThreadImpersonationToken handle to %p\n", *phToken );
            SERVER_START_REQ( set_thread_info )
            {
                req->handle   = wine_server_obj_handle( handle );
                req->token    = wine_server_obj_handle( *phToken );
                req->mask     = SET_THREAD_INFO_TOKEN;
                status = wine_server_call( req );
            }
            SERVER_END_REQ;
        }
        return status;
    case ThreadBasePriority:
        {
            const DWORD *pprio = data;
            if (length != sizeof(DWORD)) return STATUS_INVALID_PARAMETER;
            SERVER_START_REQ( set_thread_info )
            {
                req->handle   = wine_server_obj_handle( handle );
                req->priority = *pprio;
                req->mask     = SET_THREAD_INFO_PRIORITY;
                status = wine_server_call( req );
            }
            SERVER_END_REQ;
        }
        return status;
    case ThreadAffinityMask:
        {
            const ULONG_PTR affinity_mask = get_system_affinity_mask();
            ULONG_PTR req_aff;

            if (length != sizeof(ULONG_PTR)) return STATUS_INVALID_PARAMETER;
            req_aff = *(const ULONG_PTR *)data & affinity_mask;
            if (!req_aff) return STATUS_INVALID_PARAMETER;

            SERVER_START_REQ( set_thread_info )
            {
                req->handle   = wine_server_obj_handle( handle );
                req->affinity = req_aff;
                req->mask     = SET_THREAD_INFO_AFFINITY;
                status = wine_server_call( req );
            }
            SERVER_END_REQ;
        }
        return status;
    case ThreadHideFromDebugger:
        /* pretend the call succeeded to satisfy some code protectors */
        return STATUS_SUCCESS;
    case ThreadQuerySetWin32StartAddress:
        {
            const PRTL_THREAD_START_ROUTINE *entry = data;
            if (length != sizeof(PRTL_THREAD_START_ROUTINE)) return STATUS_INVALID_PARAMETER;
            SERVER_START_REQ( set_thread_info )
            {
                req->handle   = wine_server_obj_handle( handle );
                req->mask     = SET_THREAD_INFO_ENTRYPOINT;
                req->entry_point = wine_server_client_ptr( *entry );
                status = wine_server_call( req );
            }
            SERVER_END_REQ;
        }
        return status;
    case ThreadGroupInformation:
        {
            const ULONG_PTR affinity_mask = get_system_affinity_mask();
            const GROUP_AFFINITY *req_aff;

            if (length != sizeof(*req_aff)) return STATUS_INVALID_PARAMETER;
            if (!data) return STATUS_ACCESS_VIOLATION;
            req_aff = data;

            /* On Windows the request fails if the reserved fields are set */
            if (req_aff->Reserved[0] || req_aff->Reserved[1] || req_aff->Reserved[2])
                return STATUS_INVALID_PARAMETER;

            /* Wine only supports max 64 processors */
            if (req_aff->Group) return STATUS_INVALID_PARAMETER;
            if (req_aff->Mask & ~affinity_mask) return STATUS_INVALID_PARAMETER;
            if (!req_aff->Mask) return STATUS_INVALID_PARAMETER;
            SERVER_START_REQ( set_thread_info )
            {
                req->handle   = wine_server_obj_handle( handle );
                req->affinity = req_aff->Mask;
                req->mask     = SET_THREAD_INFO_AFFINITY;
                status = wine_server_call( req );
            }
            SERVER_END_REQ;
        }
        return status;
    case ThreadBasicInformation:
    case ThreadTimes:
    case ThreadPriority:
    case ThreadDescriptorTableEntry:
    case ThreadEnableAlignmentFaultFixup:
    case ThreadEventPair_Reusable:
    case ThreadPerformanceCount:
    case ThreadAmILastThread:
    case ThreadIdealProcessor:
    case ThreadPriorityBoost:
    case ThreadSetTlsArrayAddress:
    case ThreadIsIoPending:
    default:
        FIXME( "info class %d not supported yet\n", class );
        return STATUS_NOT_IMPLEMENTED;
    }
}

/******************************************************************************
 * NtGetCurrentProcessorNumber (NTDLL.@)
 *
 * Return the processor, on which the thread is running
 *
 */
ULONG WINAPI NtGetCurrentProcessorNumber(void)
{
    ULONG processor;

#if defined(__linux__) && defined(__NR_getcpu)
    int res = syscall(__NR_getcpu, &processor, NULL, NULL);
    if (res != -1) return processor;
#endif

    if (NtCurrentTeb()->Peb->NumberOfProcessors > 1)
    {
        ULONG_PTR thread_mask, processor_mask;
        NTSTATUS status;

        status = NtQueryInformationThread(GetCurrentThread(), ThreadAffinityMask,
                                          &thread_mask, sizeof(thread_mask), NULL);
        if (status == STATUS_SUCCESS)
        {
            for (processor = 0; processor < NtCurrentTeb()->Peb->NumberOfProcessors; processor++)
            {
                processor_mask = (1 << processor);
                if (thread_mask & processor_mask)
                {
                    if (thread_mask != processor_mask)
                        FIXME("need multicore support (%d processors)\n",
                              NtCurrentTeb()->Peb->NumberOfProcessors);
                    return processor;
                }
            }
        }
    }

    /* fallback to the first processor */
    return 0;
}
