/*
 * explorer.exe
 *
 * Copyright 2004 CodeWeavers, Mike Hearn
 * Copyright 2005,2006 CodeWeavers, Aric Stewart
 * Copyright 2011 Jay Yang
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

#include <stdio.h>
#include <unistd.h>

#define COBJMACROS

#include "wine/unicode.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "explorer_private.h"
#include "resource.h"

#include <initguid.h>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commoncontrols.h>
#include <commctrl.h>

WINE_DEFAULT_DEBUG_CHANNEL(explorer);

#define EXPLORER_INFO_INDEX 0

#define NAV_TOOLBAR_HEIGHT 30
#define PATHBOX_HEIGHT 24
static int nav_toolbar_height;
static int pathbox_height;

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
static int default_width;
static int default_height;


static const WCHAR EXPLORER_CLASS[] = {'E','x','p','l','o','r','e','r','W','C','l','a','s','s',0};
static const WCHAR PATH_BOX_NAME[] = {'\0'};

HINSTANCE explorer_hInstance;

typedef struct parametersTAG {
    BOOL    explorer_mode;
    WCHAR   root[MAX_PATH];
    WCHAR   selection[MAX_PATH];
} parameters_struct;

typedef struct
{
    IExplorerBrowser *browser;
    HWND main_window,path_box;
    INT rebar_height;
    LPITEMIDLIST pidl;
    IImageList *icon_list;
    DWORD advise_cookie;
} explorer_info;

enum
{
    BACK_BUTTON,FORWARD_BUTTON,UP_BUTTON
};

typedef struct
{
    IExplorerBrowserEvents IExplorerBrowserEvents_iface;
    explorer_info* info;
    LONG ref;
} IExplorerBrowserEventsImpl;

static IExplorerBrowserEventsImpl *impl_from_IExplorerBrowserEvents(IExplorerBrowserEvents *iface)
{
    return CONTAINING_RECORD(iface, IExplorerBrowserEventsImpl, IExplorerBrowserEvents_iface);
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnQueryInterface(IExplorerBrowserEvents *iface, REFIID riid, void **ppvObject)
{
    return E_NOINTERFACE;
}

static ULONG WINAPI IExplorerBrowserEventsImpl_fnAddRef(IExplorerBrowserEvents *iface)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI IExplorerBrowserEventsImpl_fnRelease(IExplorerBrowserEvents *iface)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    if(!ref)
        HeapFree(GetProcessHeap(),0,This);
    return ref;
}

static BOOL create_combobox_item(IShellFolder *folder, LPCITEMIDLIST child_pidl, IImageList *icon_list, COMBOBOXEXITEMW *item)
{
    STRRET strret;
    HRESULT hres;
    PIDLIST_ABSOLUTE parent_pidl, pidl;
    SHFILEINFOW info;
    IImageList *list;

    strret.uType=STRRET_WSTR;
    hres = IShellFolder_GetDisplayNameOf( folder, child_pidl, SHGDN_FORADDRESSBAR, &strret );
    if(SUCCEEDED(hres))
        hres = StrRetToStrW(&strret, child_pidl, &item->pszText);
    if(FAILED(hres))
    {
        WINE_WARN("Could not get name for pidl\n");
        return FALSE;
    }

    item->mask &= ~CBEIF_IMAGE;
    hres = SHGetIDListFromObject( (IUnknown *)folder, &parent_pidl );
    if (FAILED(hres)) return FALSE;

    pidl = ILCombine( parent_pidl, child_pidl );
    if (pidl)
    {
        list = (IImageList *)SHGetFileInfoW( (WCHAR *)pidl, 0, &info, sizeof(info),
                                             SHGFI_PIDL | SHGFI_SMALLICON | SHGFI_SYSICONINDEX );
        if (list)
        {
            IImageList_Release( list );
            item->iImage = info.iIcon;
            item->mask |= CBEIF_IMAGE;
        }
        ILFree( pidl );
    }
    ILFree( parent_pidl );

    return TRUE;
}

static void update_path_box(explorer_info *info)
{
    COMBOBOXEXITEMW item;
    COMBOBOXEXITEMW main_item;
    IShellFolder *desktop;
    IPersistFolder2 *persist;
    LPITEMIDLIST desktop_pidl;
    IEnumIDList *ids;

    SendMessageW(info->path_box,CB_RESETCONTENT,0,0);
    SHGetDesktopFolder(&desktop);
    IShellFolder_QueryInterface(desktop,&IID_IPersistFolder2,(void**)&persist);
    IPersistFolder2_GetCurFolder(persist,&desktop_pidl);
    IPersistFolder2_Release(persist);
    persist = NULL;
    /*Add Desktop*/
    item.iItem = -1;
    item.mask = CBEIF_TEXT | CBEIF_INDENT | CBEIF_LPARAM;
    item.iIndent = 0;
    create_combobox_item(desktop,desktop_pidl,info->icon_list,&item);
    item.lParam = (LPARAM)desktop_pidl;
    SendMessageW(info->path_box,CBEM_INSERTITEMW,0,(LPARAM)&item);
    if(ILIsEqual(info->pidl,desktop_pidl))
        main_item = item;
    else
        CoTaskMemFree(item.pszText);
    /*Add all direct subfolders of Desktop*/
    if(SUCCEEDED(IShellFolder_EnumObjects(desktop,NULL,SHCONTF_FOLDERS,&ids))
       && ids!=NULL)
    {
        LPITEMIDLIST curr_pidl=NULL;
        HRESULT hres;

        item.iIndent = 1;
        while(1)
        {
            ILFree(curr_pidl);
            curr_pidl=NULL;
            hres = IEnumIDList_Next(ids,1,&curr_pidl,NULL);
            if(FAILED(hres) || hres == S_FALSE)
                break;
            if(!create_combobox_item(desktop,curr_pidl,info->icon_list,&item))
                WINE_WARN("Could not create a combobox item\n");
            else
            {
                LPITEMIDLIST full_pidl = ILCombine(desktop_pidl,curr_pidl);
                item.lParam = (LPARAM)full_pidl;
                SendMessageW(info->path_box,CBEM_INSERTITEMW,0,(LPARAM)&item);
                if(ILIsEqual(full_pidl,info->pidl))
                    main_item = item;
                else if(ILIsParent(full_pidl,info->pidl,FALSE))
                {
                    /*add all parents of the pidl passed in*/
                    LPITEMIDLIST next_pidl = ILFindChild(full_pidl,info->pidl);
                    IShellFolder *curr_folder = NULL, *temp;
                    hres = IShellFolder_BindToObject(desktop,curr_pidl,NULL,
                                                     &IID_IShellFolder,
                                                     (void**)&curr_folder);
                    if(FAILED(hres))
                        WINE_WARN("Could not get an IShellFolder\n");
                    while(!ILIsEmpty(next_pidl))
                    {
                        LPITEMIDLIST first = ILCloneFirst(next_pidl);
                        CoTaskMemFree(item.pszText);
                        if(!create_combobox_item(curr_folder,first,
                                                 info->icon_list,&item))
                        {
                            WINE_WARN("Could not create a combobox item\n");
                            break;
                        }
                        ++item.iIndent;
                        full_pidl = ILCombine(full_pidl,first);
                        item.lParam = (LPARAM)full_pidl;
                        SendMessageW(info->path_box,CBEM_INSERTITEMW,0,(LPARAM)&item);
                        temp=NULL;
                        hres = IShellFolder_BindToObject(curr_folder,first,NULL,
                                                         &IID_IShellFolder,
                                                         (void**)&temp);
                        if(FAILED(hres))
                        {
                            WINE_WARN("Could not get an IShellFolder\n");
                            break;
                        }
                        IShellFolder_Release(curr_folder);
                        curr_folder = temp;

                        ILFree(first);
                        next_pidl = ILGetNext(next_pidl);
                    }
                    memcpy(&main_item,&item,sizeof(item));
                    if(curr_folder)
                        IShellFolder_Release(curr_folder);
                    item.iIndent = 1;
                }
                else
                    CoTaskMemFree(item.pszText);
            }
        }
        ILFree(curr_pidl);
        IEnumIDList_Release(ids);
    }
    else
        WINE_WARN("Could not enumerate the desktop\n");
    SendMessageW(info->path_box,CBEM_SETITEMW,0,(LPARAM)&main_item);
    CoTaskMemFree(main_item.pszText);
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnNavigationComplete(IExplorerBrowserEvents *iface, PCIDLIST_ABSOLUTE pidl)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    IShellFolder *parent;
    PCUITEMID_CHILD child_pidl;
    HRESULT hres;
    STRRET strret;
    WCHAR *name;

    ILFree(This->info->pidl);
    This->info->pidl = ILClone(pidl);
    update_path_box(This->info);

    hres = SHBindToParent(pidl, &IID_IShellFolder, (void **)&parent, &child_pidl);
    if (SUCCEEDED(hres))
    {
        hres = IShellFolder_GetDisplayNameOf(parent, child_pidl, SHGDN_FORADDRESSBAR, &strret);
        if (SUCCEEDED(hres))
            hres = StrRetToStrW(&strret, child_pidl, &name);
        if (SUCCEEDED(hres))
        {
            SetWindowTextW(This->info->main_window, name);
            CoTaskMemFree(name);
        }

        IShellFolder_Release(parent);
    }

    return hres;
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnNavigationFailed(IExplorerBrowserEvents *iface, PCIDLIST_ABSOLUTE pidl)
{
    return S_OK;
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnNavigationPending(IExplorerBrowserEvents *iface, PCIDLIST_ABSOLUTE pidl)
{
    return S_OK;
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnViewCreated(IExplorerBrowserEvents *iface, IShellView *psv)
{
    return S_OK;
}

static IExplorerBrowserEventsVtbl vt_IExplorerBrowserEvents =
{
    IExplorerBrowserEventsImpl_fnQueryInterface,
    IExplorerBrowserEventsImpl_fnAddRef,
    IExplorerBrowserEventsImpl_fnRelease,
    IExplorerBrowserEventsImpl_fnOnNavigationPending,
    IExplorerBrowserEventsImpl_fnOnViewCreated,
    IExplorerBrowserEventsImpl_fnOnNavigationComplete,
    IExplorerBrowserEventsImpl_fnOnNavigationFailed
};

static IExplorerBrowserEvents *make_explorer_events(explorer_info *info)
{
    IExplorerBrowserEventsImpl *ret
        = HeapAlloc(GetProcessHeap(), 0, sizeof(IExplorerBrowserEventsImpl));
    ret->IExplorerBrowserEvents_iface.lpVtbl = &vt_IExplorerBrowserEvents;
    ret->info = info;
    ret->ref = 1;
    SHGetImageList(SHIL_SMALL,&IID_IImageList,(void**)&(ret->info->icon_list));
    SendMessageW(info->path_box,CBEM_SETIMAGELIST,0,(LPARAM)ret->info->icon_list);
    return &ret->IExplorerBrowserEvents_iface;
}

static void make_explorer_window(IShellFolder* startFolder)
{
    RECT rect;
    HWND rebar,nav_toolbar;
    FOLDERSETTINGS fs;
    IExplorerBrowserEvents *events;
    explorer_info *info;
    HRESULT hres;
    WCHAR explorer_title[100];
    WCHAR pathbox_label[50];
    TBADDBITMAP bitmap_info;
    TBBUTTON nav_buttons[3];
    int hist_offset,view_offset;
    REBARBANDINFOW band_info;
    UINT dpix, dpiy;
    HDC hdc;

    memset(nav_buttons,0,sizeof(nav_buttons));

    LoadStringW(explorer_hInstance,IDS_EXPLORER_TITLE,explorer_title, ARRAY_SIZE( explorer_title ));
    LoadStringW(explorer_hInstance,IDS_PATHBOX_LABEL,pathbox_label, ARRAY_SIZE( pathbox_label ));

    hdc = GetDC(0);
    dpix = GetDeviceCaps(hdc, LOGPIXELSX);
    dpiy = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(0, hdc);
    nav_toolbar_height = MulDiv(NAV_TOOLBAR_HEIGHT, dpiy, USER_DEFAULT_SCREEN_DPI);
    pathbox_height = MulDiv(PATHBOX_HEIGHT, dpiy, USER_DEFAULT_SCREEN_DPI);
    default_width = MulDiv(DEFAULT_WIDTH, dpix, USER_DEFAULT_SCREEN_DPI);
    default_height = MulDiv(DEFAULT_HEIGHT, dpiy, USER_DEFAULT_SCREEN_DPI);

    info = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(explorer_info));
    if(!info)
    {
        WINE_ERR("Could not allocate an explorer_info struct\n");
        return;
    }
    hres = CoCreateInstance(&CLSID_ExplorerBrowser,NULL,CLSCTX_INPROC_SERVER,
                            &IID_IExplorerBrowser,(LPVOID*)&info->browser);
    if(FAILED(hres))
    {
        WINE_ERR("Could not obtain an instance of IExplorerBrowser\n");
        HeapFree(GetProcessHeap(),0,info);
        return;
    }
    info->rebar_height=0;
    info->main_window
        = CreateWindowW(EXPLORER_CLASS,explorer_title,WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT,CW_USEDEFAULT,default_width,
                        default_height,NULL,NULL,explorer_hInstance,NULL);

    fs.ViewMode = FVM_DETAILS;
    fs.fFlags = FWF_AUTOARRANGE;

    SetRect(&rect, 0, 0, default_width, default_height);
    IExplorerBrowser_Initialize(info->browser,info->main_window,&rect,&fs);
    IExplorerBrowser_SetOptions(info->browser,EBO_SHOWFRAMES);
    SetWindowLongPtrW(info->main_window,EXPLORER_INFO_INDEX,(LONG_PTR)info);

    /*setup navbar*/
    rebar = CreateWindowExW(WS_EX_TOOLWINDOW,REBARCLASSNAMEW,NULL,
                            WS_CHILD|WS_VISIBLE|RBS_VARHEIGHT|CCS_TOP|CCS_NODIVIDER,
                            0,0,0,0,info->main_window,NULL,explorer_hInstance,NULL);
    nav_toolbar
        = CreateWindowExW(TBSTYLE_EX_MIXEDBUTTONS,TOOLBARCLASSNAMEW,NULL,
                          WS_CHILD|WS_VISIBLE|TBSTYLE_FLAT,0,0,0,0,rebar,NULL,
                          explorer_hInstance,NULL);

    bitmap_info.hInst = HINST_COMMCTRL;
    bitmap_info.nID = IDB_HIST_LARGE_COLOR;
    hist_offset= SendMessageW(nav_toolbar,TB_ADDBITMAP,0,(LPARAM)&bitmap_info);
    bitmap_info.nID = IDB_VIEW_LARGE_COLOR;
    view_offset= SendMessageW(nav_toolbar,TB_ADDBITMAP,0,(LPARAM)&bitmap_info);

    nav_buttons[0].iBitmap=hist_offset+HIST_BACK;
    nav_buttons[0].idCommand=BACK_BUTTON;
    nav_buttons[0].fsState=TBSTATE_ENABLED;
    nav_buttons[0].fsStyle=BTNS_BUTTON|BTNS_AUTOSIZE;
    nav_buttons[1].iBitmap=hist_offset+HIST_FORWARD;
    nav_buttons[1].idCommand=FORWARD_BUTTON;
    nav_buttons[1].fsState=TBSTATE_ENABLED;
    nav_buttons[1].fsStyle=BTNS_BUTTON|BTNS_AUTOSIZE;
    nav_buttons[2].iBitmap=view_offset+VIEW_PARENTFOLDER;
    nav_buttons[2].idCommand=UP_BUTTON;
    nav_buttons[2].fsState=TBSTATE_ENABLED;
    nav_buttons[2].fsStyle=BTNS_BUTTON|BTNS_AUTOSIZE;
    SendMessageW(nav_toolbar,TB_BUTTONSTRUCTSIZE,sizeof(TBBUTTON),0);
    SendMessageW(nav_toolbar,TB_ADDBUTTONSW,ARRAY_SIZE( nav_buttons ),(LPARAM)nav_buttons);

    band_info.cbSize = sizeof(band_info);
    band_info.fMask = RBBIM_STYLE|RBBIM_CHILD|RBBIM_CHILDSIZE|RBBIM_SIZE;
    band_info.hwndChild = nav_toolbar;
    band_info.fStyle=RBBS_GRIPPERALWAYS|RBBS_CHILDEDGE;
    band_info.cyChild=nav_toolbar_height;
    band_info.cx=0;
    band_info.cyMinChild=nav_toolbar_height;
    band_info.cxMinChild=0;
    SendMessageW(rebar,RB_INSERTBANDW,-1,(LPARAM)&band_info);
    info->path_box = CreateWindowW(WC_COMBOBOXEXW,PATH_BOX_NAME,
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWN,
                                   0,0,default_width,pathbox_height,rebar,NULL,
                                   explorer_hInstance,NULL);
    GetWindowRect(info->path_box, &rect);
    band_info.cyChild = rect.bottom - rect.top;
    band_info.cx=0;
    band_info.cyMinChild = rect.bottom - rect.top;
    band_info.cxMinChild=0;
    band_info.fMask|=RBBIM_TEXT;
    band_info.lpText=pathbox_label;
    band_info.fStyle|=RBBS_BREAK;
    band_info.hwndChild=info->path_box;
    SendMessageW(rebar,RB_INSERTBANDW,-1,(LPARAM)&band_info);
    events = make_explorer_events(info);
    IExplorerBrowser_Advise(info->browser,events,&info->advise_cookie);
    IExplorerBrowser_BrowseToObject(info->browser,(IUnknown*)startFolder,
                                    SBSP_ABSOLUTE);
    ShowWindow(info->main_window,SW_SHOWDEFAULT);
    UpdateWindow(info->main_window);
    IExplorerBrowserEvents_Release(events);
}

static void update_window_size(explorer_info *info, int height, int width)
{
    RECT new_rect;
    new_rect.left = 0;
    new_rect.top = info->rebar_height;
    new_rect.right = width;
    new_rect.bottom = height;
    IExplorerBrowser_SetRect(info->browser,NULL,new_rect);
}

static void do_exit(int code)
{
    OleUninitialize();
    ExitProcess(code);
}

static LRESULT explorer_on_end_edit(explorer_info *info,NMCBEENDEDITW *edit_info)
{
    LPITEMIDLIST pidl = NULL;

    WINE_TRACE("iWhy=%x\n",edit_info->iWhy);
    switch(edit_info->iWhy)
    {
    case CBENF_DROPDOWN:
        if(edit_info->iNewSelection!=CB_ERR)
            pidl = (LPITEMIDLIST)SendMessageW(edit_info->hdr.hwndFrom,
                                              CB_GETITEMDATA,
                                              edit_info->iNewSelection,0);
        break;
    case CBENF_RETURN:
        {
            WCHAR path[MAX_PATH];
            HWND edit_ctrl = (HWND)SendMessageW(edit_info->hdr.hwndFrom,
                                                CBEM_GETEDITCONTROL,0,0);
            *((WORD*)path)=MAX_PATH;
            SendMessageW(edit_ctrl,EM_GETLINE,0,(LPARAM)path);
            pidl = ILCreateFromPathW(path);
            break;
        }
    case CBENF_ESCAPE:
        /*make sure the that the path box resets*/
        update_path_box(info);
        return 0;
    default:
        return 0;
    }
    if(pidl)
        IExplorerBrowser_BrowseToIDList(info->browser,pidl,SBSP_ABSOLUTE);
    if(edit_info->iWhy==CBENF_RETURN)
        ILFree(pidl);
    return 0;
}

static LRESULT update_rebar_size(explorer_info* info,NMRBAUTOSIZE *size_info)
{
    RECT new_rect;
    RECT window_rect;
    info->rebar_height = size_info->rcTarget.bottom-size_info->rcTarget.top;
    GetWindowRect(info->main_window,&window_rect);
    new_rect.left = 0;
    new_rect.top = info->rebar_height;
    new_rect.right = window_rect.right-window_rect.left;
    new_rect.bottom = window_rect.bottom-window_rect.top;
    IExplorerBrowser_SetRect(info->browser,NULL,new_rect);
    return 0;
}

static LRESULT explorer_on_notify(explorer_info* info,NMHDR* notification)
{
    WINE_TRACE("code=%i\n",notification->code);
    switch(notification->code)
    {
    case CBEN_BEGINEDIT:
        {
            WCHAR path[MAX_PATH];
            HWND edit_ctrl = (HWND)SendMessageW(notification->hwndFrom,
                                                CBEM_GETEDITCONTROL,0,0);
            SHGetPathFromIDListW(info->pidl,path);
            SetWindowTextW(edit_ctrl,path);
            break;
        }
    case CBEN_ENDEDITA:
        {
            NMCBEENDEDITA *edit_info_a = (NMCBEENDEDITA*)notification;
            NMCBEENDEDITW edit_info_w;
            edit_info_w.hdr = edit_info_a->hdr;
            edit_info_w.fChanged = edit_info_a->fChanged;
            edit_info_w.iNewSelection = edit_info_a->iNewSelection;
            MultiByteToWideChar(CP_ACP,0,edit_info_a->szText,-1,
                                edit_info_w.szText,CBEMAXSTRLEN);
            edit_info_w.iWhy = edit_info_a->iWhy;
            return explorer_on_end_edit(info,&edit_info_w);
        }
    case CBEN_ENDEDITW:
        return explorer_on_end_edit(info,(NMCBEENDEDITW*)notification);
    case CBEN_DELETEITEM:
        {
            NMCOMBOBOXEXW *entry = (NMCOMBOBOXEXW*)notification;
            if(entry->ceItem.lParam)
                ILFree((LPITEMIDLIST)entry->ceItem.lParam);
            break;
        }
    case RBN_AUTOSIZE:
        return update_rebar_size(info,(NMRBAUTOSIZE*)notification);
    default:
        break;
    }
    return 0;
}

static LRESULT CALLBACK explorer_wnd_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    explorer_info *info
        = (explorer_info*)GetWindowLongPtrW(hwnd,EXPLORER_INFO_INDEX);
    IExplorerBrowser *browser = NULL;

    WINE_TRACE("(hwnd=%p,uMsg=%u,wParam=%lx,lParam=%lx)\n",hwnd,uMsg,wParam,lParam);
    if(info)
        browser = info->browser;
    switch(uMsg)
    {
    case WM_DESTROY:
        IExplorerBrowser_Unadvise(browser,info->advise_cookie);
        IExplorerBrowser_Destroy(browser);
        IExplorerBrowser_Release(browser);
        ILFree(info->pidl);
        IImageList_Release(info->icon_list);
        HeapFree(GetProcessHeap(),0,info);
        SetWindowLongPtrW(hwnd,EXPLORER_INFO_INDEX,0);
        PostQuitMessage(0);
        break;
    case WM_QUIT:
        do_exit(wParam);
    case WM_NOTIFY:
        return explorer_on_notify(info,(NMHDR*)lParam);
    case WM_COMMAND:
        if(HIWORD(wParam)==BN_CLICKED)
        {
            switch(LOWORD(wParam))
            {
            case BACK_BUTTON:
                IExplorerBrowser_BrowseToObject(browser,NULL,SBSP_NAVIGATEBACK);
                break;
            case FORWARD_BUTTON:
                IExplorerBrowser_BrowseToObject(browser,NULL,SBSP_NAVIGATEFORWARD);
                break;
            case UP_BUTTON:
                IExplorerBrowser_BrowseToObject(browser,NULL,SBSP_PARENT);
                break;
            }
        }
        break;
    case WM_SIZE:
        update_window_size(info,HIWORD(lParam),LOWORD(lParam));
        break;
    default:
        return DefWindowProcW(hwnd,uMsg,wParam,lParam);
    }
    return 0;
}

static void register_explorer_window_class(void)
{
    WNDCLASSEXW window_class;
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.style = 0;
    window_class.cbClsExtra = 0;
    window_class.cbWndExtra = sizeof(LONG_PTR);
    window_class.lpfnWndProc = explorer_wnd_proc;
    window_class.hInstance = explorer_hInstance;
    window_class.hIcon = NULL;
    window_class.hCursor = NULL;
    window_class.hbrBackground = (HBRUSH)COLOR_BACKGROUND;
    window_class.lpszMenuName = NULL;
    window_class.lpszClassName = EXPLORER_CLASS;
    window_class.hIconSm = NULL;
    RegisterClassExW(&window_class);
}

static IShellFolder* get_starting_shell_folder(parameters_struct* params)
{
    IShellFolder* desktop,*folder;
    LPITEMIDLIST root_pidl;
    WCHAR *fullpath = NULL;
    HRESULT hres;
    DWORD size;
    WCHAR *root = params->root;

#ifdef __ANDROID__
    static WCHAR y_drive[] = {'Y',':',0};

    if (!root[0])
    {
        root = y_drive;
    }
#endif

    SHGetDesktopFolder(&desktop);
    if (!root[0])
    {
        return desktop;
    }

    size = GetFullPathNameW(root, 0, fullpath, NULL);
    if (!size)
        return desktop;
    fullpath = heap_alloc(size * sizeof(WCHAR));
    GetFullPathNameW(root, size, fullpath, NULL);

    hres = IShellFolder_ParseDisplayName(desktop,NULL,NULL,
                                         fullpath,NULL,
                                         &root_pidl,NULL);
    heap_free(fullpath);

    if(FAILED(hres))
    {
        return desktop;
    }
    hres = IShellFolder_BindToObject(desktop,root_pidl,NULL,
                                     &IID_IShellFolder,
                                     (void**)&folder);
    ILFree(root_pidl);
    if(FAILED(hres))
    {
        return desktop;
    }
    IShellFolder_Release(desktop);
    return folder;
}

static WCHAR *copy_path_string(WCHAR *target, WCHAR *source)
{
    INT i = 0;

    while (isspaceW(*source)) source++;

    if (*source == '\"')
    {
        source ++;
        while (*source && *source != '\"') target[i++] = *source++;
        target[i] = 0;
        if (*source) source++;
    }
    else
    {
        while (*source && *source != ',') target[i++] = *source++;
        target[i] = 0;
    }
    PathRemoveBackslashW(target);
    return source;
}


static void copy_path_root(LPWSTR root, LPWSTR path)
{
    LPWSTR p,p2;
    INT i = 0;

    p = path;
    while (*p!=0)
        p++;

    while (*p!='\\' && p > path)
        p--;

    if (p == path)
        return;

    p2 = path;
    while (p2 != p)
    {
        root[i] = *p2;
        i++;
        p2++;
    }
    root[i] = 0;
}

/*
 * Command Line parameters are:
 * [/n]  Opens in single-paned view for each selected items. This is default
 * [/e,] Uses Windows Explorer View
 * [/root,object] Specifies the root level of the view
 * [/select,object] parent folder is opened and specified object is selected
 */
static void parse_command_line(LPWSTR commandline,parameters_struct *parameters)
{
    static const WCHAR arg_n[] = {'/','n'};
    static const WCHAR arg_e[] = {'/','e',','};
    static const WCHAR arg_root[] = {'/','r','o','o','t',','};
    static const WCHAR arg_select[] = {'/','s','e','l','e','c','t',','};
    static const WCHAR arg_desktop[] = {'/','d','e','s','k','t','o','p'};
    static const WCHAR arg_desktop_quotes[] = {'"','/','d','e','s','k','t','o','p'};

    LPWSTR p = commandline;

    while (*p)
    {
        while (isspaceW(*p)) p++;
        if (strncmpW(p, arg_n, ARRAY_SIZE( arg_n ))==0)
        {
            parameters->explorer_mode = FALSE;
            p += ARRAY_SIZE( arg_n );
        }
        else if (strncmpW(p, arg_e, ARRAY_SIZE( arg_e ))==0)
        {
            parameters->explorer_mode = TRUE;
            p += ARRAY_SIZE( arg_e );
        }
        else if (strncmpW(p, arg_root, ARRAY_SIZE( arg_root ))==0)
        {
            p += ARRAY_SIZE( arg_root );
            p = copy_path_string(parameters->root,p);
        }
        else if (strncmpW(p, arg_select, ARRAY_SIZE( arg_select ))==0)
        {
            p += ARRAY_SIZE( arg_select );
            p = copy_path_string(parameters->selection,p);
            if (!parameters->root[0])
                copy_path_root(parameters->root,
                               parameters->selection);
        }
        else if (strncmpW(p, arg_desktop, ARRAY_SIZE( arg_desktop ))==0)
        {
            p += ARRAY_SIZE( arg_desktop );
            manage_desktop( p );  /* the rest of the command line is handled by desktop mode */
        }
        /* workaround for Worms Armageddon that hardcodes a /desktop option with quotes */
        else if (strncmpW(p, arg_desktop_quotes, ARRAY_SIZE( arg_desktop_quotes ))==0)
        {
            p += ARRAY_SIZE( arg_desktop_quotes );
            manage_desktop( p );  /* the rest of the command line is handled by desktop mode */
        }
        else
        {
            /* left over command line is generally the path to be opened */
            copy_path_string(parameters->root,p);
            break;
        }
    }
}

int WINAPI wWinMain(HINSTANCE hinstance,
                    HINSTANCE previnstance,
                    LPWSTR cmdline,
                    int cmdshow)
{

    parameters_struct   parameters;
    HRESULT hres;
    MSG msg;
    IShellFolder *folder;
    INITCOMMONCONTROLSEX init_info;

    memset(&parameters,0,sizeof(parameters));

    /*  CodeWeavers-specific hack:  We need to exclude ourselves
        from the winewrapper's wait-children process.  So we'll
        close the wait-children pipe if it is defined.  */
    {
        char *waitchild_envstring;
        unsigned int waitchild_pipeid;
        waitchild_envstring = getenv("WINE_WAIT_CHILD_PIPE");
        if (waitchild_envstring)
        {
            waitchild_pipeid = atoi(waitchild_envstring);
            if (waitchild_pipeid)
            { 
                close(waitchild_pipeid);
            }
            unsetenv("WINE_WAIT_CHILD_PIPE");
        }
    }

    explorer_hInstance = hinstance;
    parse_command_line(cmdline,&parameters);
    hres = OleInitialize(NULL);
    if(FAILED(hres))
    {
        WINE_ERR("Could not initialize COM\n");
        ExitProcess(EXIT_FAILURE);
    }
    if(parameters.root[0] && !PathIsDirectoryW(parameters.root))
        if(ShellExecuteW(NULL,NULL,parameters.root,NULL,NULL,SW_SHOWDEFAULT) > (HINSTANCE)32)
            ExitProcess(EXIT_SUCCESS);
    init_info.dwSize = sizeof(INITCOMMONCONTROLSEX);
    init_info.dwICC = ICC_USEREX_CLASSES | ICC_BAR_CLASSES | ICC_COOL_CLASSES;
    if(!InitCommonControlsEx(&init_info))
    {
        WINE_ERR("Could not initialize Comctl\n");
        ExitProcess(EXIT_FAILURE);
    }
    register_explorer_window_class();
    folder = get_starting_shell_folder(&parameters);
    make_explorer_window(folder);
    IShellFolder_Release(folder);
    while(GetMessageW( &msg, NULL, 0, 0 ) != 0)
    {
	    TranslateMessage(&msg);
	    DispatchMessageW(&msg);
    }
    return 0;
}
