/*
 * Copyright (C) 2026 WebKit W10M/UWP port.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * ARE DISCLAIMED.
 */

// W10M/UWP shims for desktop-only Win32/OLE/Shell APIs used by the Windows
// clipboard, drag-and-drop and pasteboard code. These let the desktop WebCore
// "win" sources compile inside the AppContainer.
//
// Functional shims (sound in-process semantics):
//   - ReleaseStgMedium      frees an STGMEDIUM exactly like the OLE original.
//   - RegisterClipboardFormat process-local name -> stable id registry; the
//                            WCDataObject IDataObject lives in-process, so a
//                            consistent private id per name is all it needs.
//   - Path*                  pure string helpers (extension/UNC tests).
//
// Inert shims (deferred to the WebView shell): the SYSTEM clipboard
// (Open/Close/Empty/Get/SetClipboardData and the OLE clipboard Ole*Clipboard)
// is owned by the shell, which drives Windows.ApplicationModel.DataTransfer
// .Clipboard / DataPackage on the UI thread. Here they are no-ops so copy/paste
// against the system clipboard is wired up at the shell layer, while WebCore's
// in-process pasteboard/IDataObject path is fully functional.

#pragma once

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

#include <cwchar>
#include <objidl.h>
#include <windows.h>
#include <wtf/HashMap.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

// HDROP is declared in shellapi.h, which is gated out of the AppContainer.
typedef HANDLE HDROP;

// ---- Shell clipboard format-name constants / limits -------------------------
#ifndef CFSTR_FILEDESCRIPTOR
#define CFSTR_FILEDESCRIPTORA "FileGroupDescriptor"
#define CFSTR_FILEDESCRIPTORW L"FileGroupDescriptorW"
#define CFSTR_FILEDESCRIPTOR CFSTR_FILEDESCRIPTORW
#endif
#ifndef CFSTR_FILECONTENTS
#define CFSTR_FILECONTENTS L"FileContents"
#endif
#ifndef INTERNET_MAX_URL_LENGTH
#define INTERNET_MAX_URL_LENGTH 2084
#endif
#ifndef GCT_LFNCHAR
#define GCT_INVALID 0x0000
#define GCT_LFNCHAR 0x0001
#define GCT_SHORTCHAR 0x0002
#define GCT_WILD 0x0004
#define GCT_SEPARATOR 0x0008
#endif

// ---- OLE: STGMEDIUM lifetime ------------------------------------------------

inline void ReleaseStgMedium(STGMEDIUM* medium)
{
    if (!medium)
        return;
    if (!medium->pUnkForRelease) {
        switch (medium->tymed) {
        case TYMED_HGLOBAL:
            if (medium->hGlobal)
                ::GlobalFree(medium->hGlobal);
            break;
        case TYMED_ISTREAM:
            if (medium->pstm)
                medium->pstm->Release();
            break;
        case TYMED_ISTORAGE:
            if (medium->pstg)
                medium->pstg->Release();
            break;
        default:
            break;
        }
    } else
        medium->pUnkForRelease->Release();
    medium->tymed = TYMED_NULL;
    medium->hGlobal = nullptr;
    medium->pUnkForRelease = nullptr;
}

// ---- Clipboard format registration (process-local) --------------------------

inline UINT RegisterClipboardFormatW(LPCWSTR name)
{
    static WTF::HashMap<WTF::String, unsigned> formats;
    static unsigned nextId = 0xC000; // CF_PRIVATEFIRST range
    if (!name)
        return 0;
    WTF::String key(name);
    auto it = formats.find(key);
    if (it != formats.end())
        return it->value;
    unsigned id = nextId++;
    formats.add(key, id);
    return id;
}
#define RegisterClipboardFormat RegisterClipboardFormatW

// ---- System clipboard (deferred to shell; inert here) -----------------------

inline BOOL OpenClipboard(HWND) { return FALSE; }
inline BOOL CloseClipboard() { return FALSE; }
inline BOOL EmptyClipboard() { return FALSE; }
inline BOOL IsClipboardFormatAvailable(UINT) { return FALSE; }
inline HANDLE GetClipboardData(UINT) { return nullptr; }
// Returns null so callers free the HGLOBAL they handed us (no leak).
inline HANDLE SetClipboardData(UINT, HANDLE) { return nullptr; }
inline HRESULT OleGetClipboard(IDataObject** obj) { if (obj) *obj = nullptr; return E_NOTIMPL; }
inline HRESULT OleSetClipboard(IDataObject*) { return E_NOTIMPL; }
inline HRESULT OleFlushClipboard() { return E_NOTIMPL; }

// ---- Shell path helpers (pure string ops) -----------------------------------

inline LPWSTR PathFindExtensionW(LPCWSTR path)
{
    LPCWSTR dot = nullptr;
    for (LPCWSTR p = path; p && *p; ++p) {
        if (*p == L'\\' || *p == L'/')
            dot = nullptr;
        else if (*p == L'.')
            dot = p;
    }
    LPCWSTR end = path;
    while (end && *end)
        ++end;
    return const_cast<LPWSTR>(dot ? dot : end);
}

inline BOOL PathRemoveExtensionW(LPWSTR path)
{
    LPWSTR ext = PathFindExtensionW(path);
    if (ext && *ext) {
        *ext = L'\0';
        return TRUE;
    }
    return FALSE;
}

inline BOOL PathRenameExtensionW(LPWSTR path, LPCWSTR ext)
{
    LPWSTR cur = PathFindExtensionW(path);
    if (!cur)
        return FALSE;
    ::wcscpy(cur, ext);
    return TRUE;
}

inline BOOL PathIsUNCW(LPCWSTR path)
{
    return path && path[0] == L'\\' && path[1] == L'\\';
}

inline BOOL PathFileExistsW(LPCWSTR) { return FALSE; }

inline UINT PathGetCharTypeW(WCHAR) { return 0; }

inline DWORD GetPrivateProfileStringW(LPCWSTR, LPCWSTR, LPCWSTR def, LPWSTR ret, DWORD size, LPCWSTR)
{
    if (ret && size) {
        if (def) {
            ::wcsncpy(ret, def, size - 1);
            ret[size - 1] = L'\0';
        } else
            ret[0] = L'\0';
    }
    return 0;
}

// ---- Shell file-drop (CF_HDROP / FILEGROUPDESCRIPTOR) ------------------------
// shlobj.h is not available in the AppContainer; provide the POD carriers and
// inert drag-query shims so the file-drop code compiles. File drops are wired to
// Windows.Storage StorageItems in the WebView shell.

#ifndef FD_FILESIZE
#define FD_FILESIZE 0x00000040
#endif

typedef struct _DROPFILES {
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
} DROPFILES, *LPDROPFILES;

typedef struct _FILEDESCRIPTORW {
    DWORD dwFlags;
    CLSID clsid;
    SIZEL sizel;
    POINTL pointl;
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    WCHAR cFileName[MAX_PATH];
} FILEDESCRIPTORW;

typedef struct _FILEGROUPDESCRIPTORW {
    UINT cItems;
    FILEDESCRIPTORW fgd[1];
} FILEGROUPDESCRIPTORW, *LPFILEGROUPDESCRIPTORW;

#define FILEGROUPDESCRIPTOR FILEGROUPDESCRIPTORW

inline UINT DragQueryFileW(HDROP, UINT, LPWSTR buffer, UINT bufferSize)
{
    if (buffer && bufferSize)
        buffer[0] = L'\0';
    return 0; // no files in an AppContainer HDROP
}

inline void DragFinish(HDROP) { }

// The desktop SDK exposes these via UNICODE macro aliases (shlwapi.h / shellapi.h);
// reproduce the aliases the WebCore "win" sources call by their unsuffixed names.
#define PathFindExtension PathFindExtensionW
#define PathRemoveExtension PathRemoveExtensionW
#define PathRenameExtension PathRenameExtensionW
#define PathIsUNC PathIsUNCW
#define PathFileExists PathFileExistsW
#define PathGetCharType PathGetCharTypeW
#define GetPrivateProfileString GetPrivateProfileStringW
#define DragQueryFile DragQueryFileW

// ---- Win32 input / window-message translation -------------------------------
// The "win" event sources translate HWND messages into Platform*Events. UWP
// feeds input through CoreWindow in the shell, so these are inert here (compile
// only); real input is built by the shell. WNDPROC and the GET_*_LPARAM macros
// live in winuser.h/windowsx.h, gated out of the AppContainer.

#ifndef WNDPROC
typedef LRESULT (CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
#endif

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

inline SHORT GetKeyState(int) { return 0; }
inline SHORT GetAsyncKeyState(int) { return 0; }
inline BOOL GetKeyboardState(PBYTE) { return FALSE; }
inline BOOL SetKeyboardState(LPBYTE) { return FALSE; }
inline int ToUnicodeEx(UINT, UINT, const BYTE*, LPWSTR buffer, int bufferSize, UINT, HKL)
{
    if (buffer && bufferSize)
        buffer[0] = L'\0';
    return 0;
}
inline UINT GetDoubleClickTime() { return 500; }
inline BOOL SystemParametersInfoW(UINT, UINT, PVOID, UINT) { return FALSE; }
#define SystemParametersInfo SystemParametersInfoW

inline int GetSystemMetrics(int) { return 0; }
inline HKL GetKeyboardLayout(DWORD) { return nullptr; }
inline BOOL ScreenToClient(HWND, LPPOINT) { return TRUE; } // shell maps coordinates
inline BOOL ClientToScreen(HWND, LPPOINT) { return TRUE; }
inline HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
inline VOID NotifyWinEvent(DWORD, HWND, LONG, LONG) { } // MSAA event; no-op

// ---- OLE data duplication ---------------------------------------------------

inline HANDLE OleDuplicateData(HANDLE source, CLIPFORMAT, UINT)
{
    if (!source)
        return nullptr;
    SIZE_T size = ::GlobalSize(source);
    if (!size)
        return nullptr;
    void* src = ::GlobalLock(source);
    if (!src)
        return nullptr;
    HGLOBAL copy = ::GlobalAlloc(GHND, size);
    if (copy) {
        void* dst = ::GlobalLock(copy);
        if (dst) {
            ::memcpy(dst, src, size);
            ::GlobalUnlock(copy);
        }
    }
    ::GlobalUnlock(source);
    return copy;
}

// ---- More shell path helpers ------------------------------------------------

inline BOOL PathFileExistsA(LPCSTR) { return FALSE; }
inline BOOL PathIsUNCA(LPCSTR) { return FALSE; }
inline BOOL PathAppendW(LPWSTR path, LPCWSTR more)
{
    if (!path || !more)
        return FALSE;
    size_t len = ::wcslen(path);
    if (len && path[len - 1] != L'\\' && path[len - 1] != L'/') {
        path[len++] = L'\\';
        path[len] = L'\0';
    }
    while (*more == L'\\' || *more == L'/')
        ++more;
    ::wcscat(path, more);
    return TRUE;
}
#define PathAppend PathAppendW

#endif // !WINAPI_PARTITION_DESKTOP
