/*
 * Implementation of mscoree.dll
 * Microsoft Component Object Runtime Execution Engine
 *
 * Copyright 2006 Paul Chitescu
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

#include <stdarg.h>

#define COBJMACROS
#include "wine/unicode.h"
#include "wine/library.h"
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winnls.h"
#include "winreg.h"
#include "ole2.h"
#include "ocidl.h"
#include "shellapi.h"

#include "initguid.h"
#include "msxml2.h"
#include "corerror.h"
#include "cor.h"
#include "mscoree.h"
#include "corhdr.h"
#include "cordebug.h"
#include "metahost.h"
#include "fusion.h"
#include "wine/list.h"
#include "mscoree_private.h"
#include "rpcproxy.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( mscoree );
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static HINSTANCE MSCOREE_hInstance;

typedef HRESULT (*fnCreateInstance)(REFIID riid, LPVOID *ppObj);

char *WtoA(LPCWSTR wstr)
{
    int length;
    char *result;

    length = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);

    result = HeapAlloc(GetProcessHeap(), 0, length);

    if (result)
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result, length, NULL, NULL);

    return result;
}

static BOOL get_install_root(LPWSTR install_dir)
{
    static const WCHAR dotnet_key[] = {'S','O','F','T','W','A','R','E','\\','M','i','c','r','o','s','o','f','t','\\','.','N','E','T','F','r','a','m','e','w','o','r','k','\\',0};
    static const WCHAR install_root[] = {'I','n','s','t','a','l','l','R','o','o','t',0};

    DWORD len;
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dotnet_key, 0, KEY_READ, &key))
        return FALSE;

    len = MAX_PATH;
    if (RegQueryValueExW(key, install_root, 0, NULL, (LPBYTE)install_dir, &len))
    {
        RegCloseKey(key);
        return FALSE;
    }
    RegCloseKey(key);

    return TRUE;
}

typedef struct mscorecf
{
    IClassFactory    IClassFactory_iface;
    LONG ref;

    fnCreateInstance pfnCreateInstance;

    CLSID clsid;
} mscorecf;

static inline mscorecf *impl_from_IClassFactory( IClassFactory *iface )
{
    return CONTAINING_RECORD(iface, mscorecf, IClassFactory_iface);
}

static HRESULT WINAPI mscorecf_QueryInterface(IClassFactory *iface, REFIID riid, LPVOID *ppobj )
{
    TRACE("%s %p\n", debugstr_guid(riid), ppobj);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IClassFactory))
    {
        IClassFactory_AddRef( iface );
        *ppobj = iface;
        return S_OK;
    }

    ERR("interface %s not implemented\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI mscorecf_AddRef(IClassFactory *iface )
{
    mscorecf *This = impl_from_IClassFactory(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("%p ref=%u\n", This, ref);

    return ref;
}

static ULONG WINAPI mscorecf_Release(IClassFactory *iface )
{
    mscorecf *This = impl_from_IClassFactory(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("%p ref=%u\n", This, ref);

    if (ref == 0)
    {
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

static HRESULT WINAPI mscorecf_CreateInstance(IClassFactory *iface,LPUNKNOWN pOuter,
                            REFIID riid, LPVOID *ppobj )
{
    mscorecf *This = impl_from_IClassFactory( iface );
    HRESULT hr;
    IUnknown *punk;

    TRACE("%p %s %p\n", pOuter, debugstr_guid(riid), ppobj );

    *ppobj = NULL;

    if (pOuter)
        return CLASS_E_NOAGGREGATION;

    hr = This->pfnCreateInstance( &This->clsid, (LPVOID*) &punk );
    if (SUCCEEDED(hr))
    {
        hr = IUnknown_QueryInterface( punk, riid, ppobj );

        IUnknown_Release( punk );
    }
    else
    {
        WARN("Cannot create an instance object. 0x%08x\n", hr);
    }
    return hr;
}

static HRESULT WINAPI mscorecf_LockServer(IClassFactory *iface, BOOL dolock)
{
    FIXME("(%p)->(%d),stub!\n",iface,dolock);
    return S_OK;
}

static const struct IClassFactoryVtbl mscorecf_vtbl =
{
    mscorecf_QueryInterface,
    mscorecf_AddRef,
    mscorecf_Release,
    mscorecf_CreateInstance,
    mscorecf_LockServer
};

HRESULT WINAPI CorBindToRuntimeHost(LPCWSTR pwszVersion, LPCWSTR pwszBuildFlavor,
                                    LPCWSTR pwszHostConfigFile, VOID *pReserved,
                                    DWORD startupFlags, REFCLSID rclsid,
                                    REFIID riid, LPVOID *ppv)
{
    HRESULT ret;
    ICLRRuntimeInfo *info;

    TRACE("(%s, %s, %s, %p, %d, %s, %s, %p)\n", debugstr_w(pwszVersion),
          debugstr_w(pwszBuildFlavor), debugstr_w(pwszHostConfigFile), pReserved,
          startupFlags, debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    *ppv = NULL;

    ret = get_runtime_info(NULL, pwszVersion, pwszHostConfigFile, startupFlags, 0, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        ret = ICLRRuntimeInfo_GetInterface(info, rclsid, riid, ppv);

        ICLRRuntimeInfo_Release(info);
    }

    return ret;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    TRACE("(%p, %d, %p)\n", hinstDLL, fdwReason, lpvReserved);

    MSCOREE_hInstance = hinstDLL;

    switch (fdwReason)
    {
    case DLL_WINE_PREATTACH:
        return FALSE;  /* prefer native version */
    case DLL_PROCESS_ATTACH:
        runtimehost_init();
        DisableThreadLibraryCalls(hinstDLL);
        break;
    case DLL_PROCESS_DETACH:
        expect_no_runtimes();
        if (lpvReserved) break; /* process is terminating */
        runtimehost_uninit();
        break;
    }
    return TRUE;
}

__int32 WINAPI _CorExeMain2(PBYTE ptrMemory, DWORD cntMemory, LPWSTR imageName, LPWSTR loaderName, LPWSTR cmdLine)
{
    TRACE("(%p, %u, %s, %s, %s)\n", ptrMemory, cntMemory, debugstr_w(imageName), debugstr_w(loaderName), debugstr_w(cmdLine));
    FIXME("Directly running .NET applications not supported.\n");
    return -1;
}

void WINAPI CorExitProcess(int exitCode)
{
    TRACE("(%x)\n", exitCode);
    CLRMetaHost_ExitProcess(0, exitCode);
}

VOID WINAPI _CorImageUnloading(PVOID imageBase)
{
    TRACE("(%p): stub\n", imageBase);
}

HRESULT WINAPI _CorValidateImage(PVOID* imageBase, LPCWSTR imageName)
{
    TRACE("(%p, %s): stub\n", imageBase, debugstr_w(imageName));
    return E_FAIL;
}

HRESULT WINAPI GetCORSystemDirectory(LPWSTR pbuffer, DWORD cchBuffer, DWORD *dwLength)
{
    ICLRRuntimeInfo *info;
    HRESULT ret;

    TRACE("(%p, %d, %p)!\n", pbuffer, cchBuffer, dwLength);

    if (!dwLength || !pbuffer)
        return E_POINTER;

    ret = get_runtime_info(NULL, NULL, NULL, 0, RUNTIME_INFO_UPGRADE_VERSION, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        *dwLength = cchBuffer;
        ret = ICLRRuntimeInfo_GetRuntimeDirectory(info, pbuffer, dwLength);

        ICLRRuntimeInfo_Release(info);
    }

    return ret;
}

HRESULT WINAPI GetCORVersion(LPWSTR pbuffer, DWORD cchBuffer, DWORD *dwLength)
{
    ICLRRuntimeInfo *info;
    HRESULT ret;

    TRACE("(%p, %d, %p)!\n", pbuffer, cchBuffer, dwLength);

    if (!dwLength || !pbuffer)
        return E_POINTER;

    ret = get_runtime_info(NULL, NULL, NULL, 0, RUNTIME_INFO_UPGRADE_VERSION, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        *dwLength = cchBuffer;
        ret = ICLRRuntimeInfo_GetVersionString(info, pbuffer, dwLength);

        ICLRRuntimeInfo_Release(info);
    }

    return ret;
}

HRESULT WINAPI CorIsLatestSvc(int *unk1, int *unk2)
{
    ERR_(winediag)("If this function is called, it is likely the result of a broken .NET installation\n");

    if (!unk1 || !unk2)
        return E_POINTER;

    return S_OK;
}

HRESULT WINAPI GetRequestedRuntimeInfo(LPCWSTR pExe, LPCWSTR pwszVersion, LPCWSTR pConfigurationFile,
    DWORD startupFlags, DWORD runtimeInfoFlags, LPWSTR pDirectory, DWORD dwDirectory, DWORD *dwDirectoryLength,
    LPWSTR pVersion, DWORD cchBuffer, DWORD *dwlength)
{
    HRESULT ret;
    ICLRRuntimeInfo *info;
    DWORD length_dummy;

    TRACE("(%s, %s, %s, 0x%08x, 0x%08x, %p, 0x%08x, %p, %p, 0x%08x, %p)\n", debugstr_w(pExe),
          debugstr_w(pwszVersion), debugstr_w(pConfigurationFile), startupFlags, runtimeInfoFlags, pDirectory,
          dwDirectory, dwDirectoryLength, pVersion, cchBuffer, dwlength);

    if (!dwDirectoryLength) dwDirectoryLength = &length_dummy;

    if (!dwlength) dwlength = &length_dummy;

    ret = get_runtime_info(pExe, pwszVersion, pConfigurationFile, startupFlags, runtimeInfoFlags, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        *dwlength = cchBuffer;
        ret = ICLRRuntimeInfo_GetVersionString(info, pVersion, dwlength);

        if (SUCCEEDED(ret))
        {
            if(pwszVersion)
                pVersion[0] = pwszVersion[0];

            *dwDirectoryLength = dwDirectory;
            ret = ICLRRuntimeInfo_GetRuntimeDirectory(info, pDirectory, dwDirectoryLength);
        }

        ICLRRuntimeInfo_Release(info);
    }

    return ret;
}

HRESULT WINAPI GetRequestedRuntimeVersion(LPWSTR pExe, LPWSTR pVersion, DWORD cchBuffer, DWORD *dwlength)
{
    TRACE("(%s, %p, %d, %p)\n", debugstr_w(pExe), pVersion, cchBuffer, dwlength);

    if(!dwlength)
        return E_POINTER;

    return GetRequestedRuntimeInfo(pExe, NULL, NULL, 0, 0, NULL, 0, NULL, pVersion, cchBuffer, dwlength);
}

HRESULT WINAPI GetRealProcAddress(LPCSTR procname, void **ppv)
{
    FIXME("(%s, %p)\n", debugstr_a(procname), ppv);
    return CLR_E_SHIM_RUNTIMEEXPORT;
}

HRESULT WINAPI GetFileVersion(LPCWSTR szFilename, LPWSTR szBuffer, DWORD cchBuffer, DWORD *dwLength)
{
    TRACE("(%s, %p, %d, %p)\n", debugstr_w(szFilename), szBuffer, cchBuffer, dwLength);

    if (!szFilename || !dwLength)
        return E_POINTER;

    *dwLength = cchBuffer;
    return CLRMetaHost_GetVersionFromFile(0, szFilename, szBuffer, dwLength);
}

HRESULT WINAPI LoadLibraryShim( LPCWSTR szDllName, LPCWSTR szVersion, LPVOID pvReserved, HMODULE * phModDll)
{
    HRESULT ret=S_OK;
    WCHAR dll_filename[MAX_PATH];
    WCHAR version[MAX_PATH];
    static const WCHAR default_version[] = {'v','1','.','1','.','4','3','2','2',0};
    static const WCHAR slash[] = {'\\',0};
    DWORD dummy;

    TRACE("(%p %s, %p, %p, %p)\n", szDllName, debugstr_w(szDllName), szVersion, pvReserved, phModDll);

    if (!szDllName || !phModDll)
        return E_POINTER;

    if (!get_install_root(dll_filename))
    {
        ERR("error reading registry key for installroot\n");
        dll_filename[0] = 0;
    }
    else
    {
        if (!szVersion)
        {
            ret = GetCORVersion(version, MAX_PATH, &dummy);
            if (SUCCEEDED(ret))
                szVersion = version;
            else
                szVersion = default_version;
        }
        strcatW(dll_filename, szVersion);
        strcatW(dll_filename, slash);
    }

    strcatW(dll_filename, szDllName);

    *phModDll = LoadLibraryW(dll_filename);

    return *phModDll ? S_OK : E_HANDLE;
}

HRESULT WINAPI LockClrVersion(FLockClrVersionCallback hostCallback, FLockClrVersionCallback *pBeginHostSetup, FLockClrVersionCallback *pEndHostSetup)
{
    FIXME("(%p %p %p): stub\n", hostCallback, pBeginHostSetup, pEndHostSetup);
    return S_OK;
}

HRESULT WINAPI CoInitializeCor(DWORD fFlags)
{
    FIXME("(0x%08x): stub\n", fFlags);
    return S_OK;
}

HRESULT WINAPI GetAssemblyMDImport(LPCWSTR szFileName, REFIID riid, IUnknown **ppIUnk)
{
    FIXME("(%p %s, %s, %p): stub\n", szFileName, debugstr_w(szFileName), debugstr_guid(riid), *ppIUnk);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

HRESULT WINAPI GetVersionFromProcess(HANDLE hProcess, LPWSTR pVersion, DWORD cchBuffer, DWORD *dwLength)
{
    FIXME("(%p, %p, %d, %p): stub\n", hProcess, pVersion, cchBuffer, dwLength);
    return E_NOTIMPL;
}

HRESULT WINAPI LoadStringRCEx(LCID culture, UINT resId, LPWSTR pBuffer, int iBufLen, int bQuiet, int* pBufLen)
{
    HRESULT res = S_OK;
    if ((iBufLen <= 0) || !pBuffer)
        return E_INVALIDARG;
    pBuffer[0] = 0;
    if (resId) {
        FIXME("(%d, %x, %p, %d, %d, %p): semi-stub\n", culture, resId, pBuffer, iBufLen, bQuiet, pBufLen);
        res = E_NOTIMPL;
    }
    else
        res = E_FAIL;
    if (pBufLen)
        *pBufLen = lstrlenW(pBuffer);
    return res;
}

HRESULT WINAPI LoadStringRC(UINT resId, LPWSTR pBuffer, int iBufLen, int bQuiet)
{
    return LoadStringRCEx(-1, resId, pBuffer, iBufLen, bQuiet, NULL);
}

HRESULT WINAPI CorBindToRuntimeEx(LPWSTR szVersion, LPWSTR szBuildFlavor, DWORD nflags, REFCLSID rslsid,
                                  REFIID riid, LPVOID *ppv)
{
    HRESULT ret;
    ICLRRuntimeInfo *info;

    TRACE("%s %s %d %s %s %p\n", debugstr_w(szVersion), debugstr_w(szBuildFlavor), nflags, debugstr_guid( rslsid ),
          debugstr_guid( riid ), ppv);

    *ppv = NULL;

    ret = get_runtime_info(NULL, szVersion, NULL, nflags, RUNTIME_INFO_UPGRADE_VERSION, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        ret = ICLRRuntimeInfo_GetInterface(info, rslsid, riid, ppv);

        ICLRRuntimeInfo_Release(info);
    }

    return ret;
}

HRESULT WINAPI CorBindToCurrentRuntime(LPCWSTR filename, REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    HRESULT ret;
    ICLRRuntimeInfo *info;

    TRACE("(%s, %s, %s, %p)\n", debugstr_w(filename), debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    *ppv = NULL;

    ret = get_runtime_info(NULL, NULL, filename, 0, RUNTIME_INFO_UPGRADE_VERSION, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        ret = ICLRRuntimeInfo_GetInterface(info, rclsid, riid, ppv);

        ICLRRuntimeInfo_Release(info);
    }

    return ret;
}

STDAPI ClrCreateManagedInstance(LPCWSTR pTypeName, REFIID riid, void **ppObject)
{
    HRESULT ret;
    ICLRRuntimeInfo *info;
    RuntimeHost *host;
    MonoObject *obj;
    IUnknown *unk;

    TRACE("(%s,%s,%p)\n", debugstr_w(pTypeName), debugstr_guid(riid), ppObject);

    /* FIXME: How to determine which runtime version to use? */
    ret = get_runtime_info(NULL, NULL, NULL, 0, RUNTIME_INFO_UPGRADE_VERSION, TRUE, &info);

    if (SUCCEEDED(ret))
    {
        ret = ICLRRuntimeInfo_GetRuntimeHost(info, &host);

        ICLRRuntimeInfo_Release(info);
    }

    if (SUCCEEDED(ret))
        ret = RuntimeHost_CreateManagedInstance(host, pTypeName, NULL, &obj);

    if (SUCCEEDED(ret))
        ret = RuntimeHost_GetIUnknownForObject(host, obj, &unk);

    if (SUCCEEDED(ret))
    {
        ret = IUnknown_QueryInterface(unk, riid, ppObject);
        IUnknown_Release(unk);
    }

    return ret;
}

BOOL WINAPI StrongNameSignatureVerification(LPCWSTR filename, DWORD inFlags, DWORD* pOutFlags)
{
    FIXME("(%s, 0x%X, %p): stub\n", debugstr_w(filename), inFlags, pOutFlags);
    return FALSE;
}

BOOL WINAPI StrongNameSignatureVerificationEx(LPCWSTR filename, BOOL forceVerification, BOOL* pVerified)
{
    FIXME("(%s, %u, %p): stub\n", debugstr_w(filename), forceVerification, pVerified);
    return FALSE;
}

HRESULT WINAPI CreateDebuggingInterfaceFromVersion(int nDebugVersion, LPCWSTR version, IUnknown **ppv)
{
    static const WCHAR v2_0[] = {'v','2','.','0','.','5','0','7','2','7',0};
    HRESULT hr = E_FAIL;
    ICLRRuntimeInfo *runtimeinfo;

    if(nDebugVersion < 1 || nDebugVersion > 4)
        return E_INVALIDARG;

    TRACE("(%d %s, %p): stub\n", nDebugVersion, debugstr_w(version), ppv);

    if(!ppv)
        return E_INVALIDARG;

    *ppv = NULL;

    if(strcmpW(version, v2_0) != 0)
    {
        FIXME("Currently .NET Version '%s' not support.\n", debugstr_w(version));
        return E_INVALIDARG;
    }

    if(nDebugVersion != 3)
        return E_INVALIDARG;

    hr = CLRMetaHost_GetRuntime(0, version, &IID_ICLRRuntimeInfo, (void**)&runtimeinfo);
    if(hr == S_OK)
    {
        hr = ICLRRuntimeInfo_GetInterface(runtimeinfo, &CLSID_CLRDebuggingLegacy, &IID_ICorDebug, (void**)ppv);

        ICLRRuntimeInfo_Release(runtimeinfo);
    }

    if(!*ppv)
        return E_FAIL;

    return hr;
}

HRESULT WINAPI CLRCreateInstance(REFCLSID clsid, REFIID riid, LPVOID *ppInterface)
{
    TRACE("(%s,%s,%p)\n", debugstr_guid(clsid), debugstr_guid(riid), ppInterface);

    if (IsEqualGUID(clsid, &CLSID_CLRMetaHost))
        return CLRMetaHost_CreateInstance(riid, ppInterface);
    if (IsEqualGUID(clsid, &CLSID_CLRMetaHostPolicy))
        return CLRMetaHostPolicy_CreateInstance(riid, ppInterface);

    FIXME("not implemented for class %s\n", debugstr_guid(clsid));

    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI CreateInterface(REFCLSID clsid, REFIID riid, LPVOID *ppInterface)
{
    TRACE("(%s,%s,%p)\n", debugstr_guid(clsid), debugstr_guid(riid), ppInterface);

    return CLRCreateInstance(clsid, riid, ppInterface);
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    mscorecf *This;
    HRESULT hr;

    TRACE("(%s, %s, %p): stub\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    if(!ppv)
        return E_INVALIDARG;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(mscorecf));

    This->IClassFactory_iface.lpVtbl = &mscorecf_vtbl;
    This->pfnCreateInstance = create_monodata;
    This->ref = 1;
    This->clsid = *rclsid;

    hr = IClassFactory_QueryInterface( &This->IClassFactory_iface, riid, ppv );
    IClassFactory_Release(&This->IClassFactory_iface);

    return hr;
}

static void parse_msi_version_string(const char *version, int *parts)
{
    const char *minor_start, *build_start;

    parts[0] = atoi(version);

    parts[1] = parts[2] = 0;

    minor_start = strchr(version, '.');
    if (minor_start)
    {
        minor_start++;
        parts[1] = atoi(minor_start);

        build_start = strchr(minor_start, '.');
        if (build_start)
            parts[2] = atoi(build_start+1);
    }
}

static BOOL install_wine_mono(void)
{
    BOOL is_wow64 = FALSE;
    HMODULE hmsi;
    UINT (WINAPI *pMsiEnumRelatedProductsA)(LPCSTR,DWORD,DWORD,LPSTR);
    UINT (WINAPI *pMsiGetProductInfoA)(LPCSTR,LPCSTR,LPSTR,DWORD*);
    char versionstringbuf[15];
    char productcodebuf[39];
    UINT res;
    DWORD buffer_size;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    WCHAR app[MAX_PATH];
    WCHAR *args;
    LONG len;
    BOOL ret;

    static const char* mono_version = "4.7.5";
    static const char* mono_upgrade_code = "{DE624609-C6B5-486A-9274-EF0B854F6BC5}";

    static const WCHAR controlW[] = {'\\','c','o','n','t','r','o','l','.','e','x','e',0};
    static const WCHAR argsW[] =
        {' ','a','p','p','w','i','z','.','c','p','l',' ','i','n','s','t','a','l','l','_','m','o','n','o',0};

    IsWow64Process(GetCurrentProcess(), &is_wow64);

    if (is_wow64)
    {
        TRACE("not installing mono in wow64 process\n");
        return TRUE;
    }

    hmsi = LoadLibraryA("msi");

    if (!hmsi)
    {
        ERR("couldn't load msi.dll\n");
        return FALSE;
    }

    pMsiEnumRelatedProductsA = (void*)GetProcAddress(hmsi, "MsiEnumRelatedProductsA");

    res = pMsiEnumRelatedProductsA(mono_upgrade_code, 0, 0, productcodebuf);

    if (res == ERROR_SUCCESS)
    {
        pMsiGetProductInfoA = (void*)GetProcAddress(hmsi, "MsiGetProductInfoA");

        buffer_size = sizeof(versionstringbuf);

        res = pMsiGetProductInfoA(productcodebuf, "VersionString", versionstringbuf, &buffer_size);
    }
    else if (res != ERROR_NO_MORE_ITEMS)
    {
        ERR("MsiEnumRelatedProducts failed, err=%u\n", res);
    }

    FreeLibrary(hmsi);

    if (res == ERROR_SUCCESS)
    {
        int current_version[3], wanted_version[3], i;

        TRACE("found installed version %s\n", versionstringbuf);

        parse_msi_version_string(versionstringbuf, current_version);
        parse_msi_version_string(mono_version, wanted_version);

        for (i=0; i<3; i++)
        {
            if (current_version[i] < wanted_version[i])
                break;
            else if (current_version[i] > wanted_version[i])
            {
                TRACE("installed version is newer than %s, quitting\n", mono_version);
                return TRUE;
            }
        }

        if (i == 3)
        {
            TRACE("version %s is already installed, quitting\n", mono_version);
            return TRUE;
        }
    }

    len = GetSystemDirectoryW(app, MAX_PATH - ARRAY_SIZE(controlW));
    memcpy(app+len, controlW, sizeof(controlW));

    args = HeapAlloc(GetProcessHeap(), 0, (len*sizeof(WCHAR) + sizeof(controlW) + sizeof(argsW)));
    if(!args)
        return FALSE;

    memcpy(args, app, len*sizeof(WCHAR) + sizeof(controlW));
    memcpy(args + len + ARRAY_SIZE(controlW) - 1, argsW, sizeof(argsW));

    TRACE("starting %s\n", debugstr_w(args));

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    ret = CreateProcessW(app, args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    HeapFree(GetProcessHeap(), 0, args);
    if (ret) {
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
    }

    return ret;
}

/* CROSSOVER HACK bug 10194 */
void WINAPI wine_install_mono(HWND hwnd, HINSTANCE hinst, LPSTR cmdline, int cmdshow)
{
    install_wine_mono();
}

HRESULT WINAPI DllRegisterServer(void)
{
    install_wine_mono();

    return __wine_register_resources( MSCOREE_hInstance );
}

HRESULT WINAPI DllUnregisterServer(void)
{
    return __wine_unregister_resources( MSCOREE_hInstance );
}

HRESULT WINAPI DllCanUnloadNow(VOID)
{
    return S_FALSE;
}

void WINAPI CoEEShutDownCOM(void)
{
    FIXME("stub.\n");
}

INT WINAPI ND_RU1( const void *ptr, INT offset )
{
    return *((const BYTE *)ptr + offset);
}

INT WINAPI ND_RI2( const void *ptr, INT offset )
{
    return *(const SHORT *)((const BYTE *)ptr + offset);
}

INT WINAPI ND_RI4( const void *ptr, INT offset )
{
    return *(const INT *)((const BYTE *)ptr + offset);
}

INT64 WINAPI ND_RI8( const void *ptr, INT offset )
{
    return *(const INT64 *)((const BYTE *)ptr + offset);
}

void WINAPI ND_WU1( void *ptr, INT offset, BYTE val )
{
    *((BYTE *)ptr + offset) = val;
}

void WINAPI ND_WI2( void *ptr, INT offset, SHORT val )
{
    *(SHORT *)((BYTE *)ptr + offset) = val;
}

void WINAPI ND_WI4( void *ptr, INT offset, INT val )
{
    *(INT *)((BYTE *)ptr + offset) = val;
}

void WINAPI ND_WI8( void *ptr, INT offset, INT64 val )
{
    *(INT64 *)((BYTE *)ptr + offset) = val;
}

void WINAPI ND_CopyObjDst( const void *src, void *dst, INT offset, INT size )
{
    memcpy( (BYTE *)dst + offset, src, size );
}

void WINAPI ND_CopyObjSrc( const void *src, INT offset, void *dst, INT size )
{
    memcpy( dst, (const BYTE *)src + offset, size );
}
