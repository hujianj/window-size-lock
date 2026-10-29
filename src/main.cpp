#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

// Entire application and WinEvent callbacks run on one message-loop thread.
enum { ID_LIST=101, ID_LOCK, ID_UNLOCK, ID_REFRESH, ID_PICK, ID_HIDE, ID_EXIT,
       ID_TRAY_OPEN=201, ID_TRAY_UNLOCK, ID_TRAY_EXIT };
constexpr UINT WM_CHECK = WM_APP+1, WM_TRAY = WM_APP+2;
constexpr UINT_PTR TIMER_WATCH=1, TIMER_SETTLE=2;
struct Candidate { HWND hwnd; DWORD pid; };
static HINSTANCE instance;
static HWND mainWindow, listBox, targetLabel, sizeLabel, statusLabel, lockButton,
            unlockButton, pickerButton, refreshButton, hideButton, exitButton;
static HWND brandLabel, hintLabel, badgeLabel, targetCaption, wantedCaption,
            currentCaption, currentSizeLabel, listCaption, shortcutLabel;
static HFONT font, titleFont, targetFont, metricFont, smallFont;
static HBRUSH backgroundBrush, cardBrush;
static constexpr COLORREF Background=RGB(245,247,251), Card=RGB(255,255,255),
    Ink=RGB(31,41,55), Muted=RGB(107,114,128), Accent=RGB(37,99,235),
    Border=RGB(226,232,240), Selection=RGB(239,246,255);
enum class BadgeState { Ready, Active, Paused, Picking };
static BadgeState badgeState=BadgeState::Ready;
static HWND hoveredButton;
static Candidate candidates[512];
static int candidateCount;
static HWND target;
static DWORD targetPid;
static HANDLE targetProcess;
static int wantedWidth, wantedHeight;
static bool locked, queued, destroyed, picking, inCorrection, trayAdded;
static HWINEVENTHOOK objectHook, systemHook;
static ULONGLONG lastAttempt, mismatchSince, lastLogAt;
static RECT lastLoggedRect;
static bool haveLoggedRect, lastLoggedMin, lastLoggedMax;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static UINT taskbarCreated;
static HICON appIcon;
static int dpi=96;
static bool captureHotkey, unlockHotkey;
static bool pickerLowered;
static POINT pickerStart;
using DpiForWindowFunction=UINT (WINAPI*)(HWND);
using AdjustForDpiFunction=BOOL (WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
static DpiForWindowFunction dpiForWindow;
static AdjustForDpiFunction adjustForDpi;

static void InitializeDpi() {
    HMODULE user32=GetModuleHandleW(L"user32.dll");
    using SetContextFunction=BOOL (WINAPI*)(HANDLE);
    auto setContext=reinterpret_cast<SetContextFunction>(GetProcAddress(user32,"SetProcessDpiAwarenessContext"));
    dpiForWindow=reinterpret_cast<DpiForWindowFunction>(GetProcAddress(user32,"GetDpiForWindow"));
    adjustForDpi=reinterpret_cast<AdjustForDpiFunction>(GetProcAddress(user32,"AdjustWindowRectExForDpi"));
    if(setContext) { if(!setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE); }
    else SetProcessDPIAware();
}

static int Px(int n) { return MulDiv(n,dpi,96); }
static void SetText(HWND h, const wchar_t* text) {
    wchar_t current[1024];
    // Rewriting identical text generates paint/accessibility notifications.
    // A stationary locked window should require no UI work on watchdog ticks.
    if(GetWindowTextW(h,current,1024)>0 && lstrcmpW(current,text)==0) return;
    if(!text[0] && GetWindowTextLengthW(h)==0) return;
    SetWindowTextW(h,text);
}

static void Log(const wchar_t* message) {
    if(logFile==INVALID_HANDLE_VALUE) return;
    wchar_t line[1024];
    StringCchPrintfW(line,1024,L"%llu %s\r\n",GetTickCount64(),message);
    char utf8[4096];
    int count=WideCharToMultiByte(CP_UTF8,0,line,-1,utf8,sizeof utf8,nullptr,nullptr);
    if(count>1) { DWORD written; WriteFile(logFile,utf8,count-1,&written,nullptr); }
}

static void StartDiagnostics() {
    // Local test opt-in. Normal use creates no log or configuration file.
    wchar_t marker[MAX_PATH+32];
    GetModuleFileNameW(nullptr,marker,MAX_PATH);
    StringCchCatW(marker,MAX_PATH+32,L".diagnostics");
    HANDLE file=CreateFileW(marker,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) return;
    char bytes[4096]={}; DWORD got=0;
    ReadFile(file,bytes,sizeof(bytes)-1,&got,nullptr); CloseHandle(file);
    wchar_t path[2048]={};
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,got,path,2047)) return;
    for(wchar_t* p=path;*p;++p) if(*p==L'\r'||*p==L'\n') { *p=0; break; }
    if(!path[0]) return;
    logFile=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    Log(L"START version=1.1.0");
}

static RECT BadgeRect() {
    RECT r; GetClientRect(mainWindow,&r);
    return {r.right-Px(104),Px(22),r.right-Px(24),Px(48)};
}

static void Status(const wchar_t* message) {
    SetText(statusLabel,message);
    BadgeState next=picking?BadgeState::Picking:!locked?BadgeState::Ready:
        (IsIconic(target)||!IsWindowVisible(target))?BadgeState::Paused:BadgeState::Active;
    SetText(badgeLabel,next==BadgeState::Picking?L"选取中":next==BadgeState::Ready?L"未固定":
        next==BadgeState::Paused?L"已暂停":L"固定中");
    if(next!=badgeState) {
        badgeState=next; RECT r=BadgeRect(); InvalidateRect(mainWindow,&r,FALSE);
        InvalidateRect(badgeLabel,nullptr,TRUE);
    }
}

static void UpdateTray() {
    if(!trayAdded) return;
    NOTIFYICONDATAW n={sizeof n}; n.hWnd=mainWindow; n.uID=1; n.uFlags=NIF_TIP;
    if(locked) StringCchPrintfW(n.szTip,128,L"窗口尺寸固定：%d × %d",wantedWidth,wantedHeight);
    else StringCchCopyW(n.szTip,128,L"窗口尺寸固定：未固定");
    Shell_NotifyIconW(NIM_MODIFY,&n);
}

static void AddTray() {
    NOTIFYICONDATAW n={sizeof n};
    n.hWnd=mainWindow; n.uID=1; n.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    n.hIcon=appIcon; n.uCallbackMessage=WM_TRAY;
    StringCchCopyW(n.szTip,128,L"窗口尺寸固定");
    trayAdded=Shell_NotifyIconW(NIM_ADD,&n)!=FALSE;
    UpdateTray();
}

static void ShowMain() {
    ShowWindow(mainWindow,SW_RESTORE);
    SetWindowPos(mainWindow,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    SetForegroundWindow(mainWindow);
}

static void StopHooks() {
    if(objectHook) UnhookWinEvent(objectHook);
    if(systemHook) UnhookWinEvent(systemHook);
    objectHook=systemHook=nullptr;
}

static void Unlock(const wchar_t* reason) {
    bool wasLocked=locked;
    locked=false; target=nullptr; targetPid=0; queued=false; destroyed=false;
    StopHooks(); KillTimer(mainWindow,TIMER_SETTLE);
    if(targetProcess) CloseHandle(targetProcess);
    targetProcess=nullptr;
    EnableWindow(unlockButton,FALSE);
    SetText(targetLabel,L"还没有选取窗口");
    SetText(sizeLabel,L"— × —"); SetText(currentSizeLabel,L"— × —");
    Status(reason); UpdateTray();
    if(wasLocked) Log(L"UNLOCK");
}

static void QueueCheck() {
    if(locked && !queued) { queued=true; PostMessageW(mainWindow,WM_CHECK,0,0); }
}

static void CALLBACK WindowEvent(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                LONG object, LONG child, DWORD, DWORD) {
    if(!locked || hwnd!=target) return;
    if(event>=EVENT_OBJECT_CREATE && (object!=OBJID_WINDOW || child!=CHILDID_SELF)) return;
    if(event==EVENT_OBJECT_DESTROY) destroyed=true;
    QueueCheck();
}

static bool TargetValid() {
    if(destroyed || !target || !IsWindow(target)) return false;
    DWORD pid=0; GetWindowThreadProcessId(target,&pid);
    if(pid!=targetPid) return false;
    if(targetProcess && WaitForSingleObject(targetProcess,0)==WAIT_OBJECT_0) return false;
    return true;
}

static void Observe(const RECT& r, bool minimized, bool maximized) {
    wchar_t text[512];
    StringCchPrintfW(text,512,L"%d × %d",wantedWidth,wantedHeight);
    SetText(sizeLabel,text);
    if(minimized) StringCchCopyW(text,512,L"已最小化");
    else StringCchPrintfW(text,512,L"%ld × %ld",r.right-r.left,r.bottom-r.top);
    SetText(currentSizeLabel,text);
    if(logFile!=INVALID_HANDLE_VALUE && (!haveLoggedRect ||
       !EqualRect(&r,&lastLoggedRect) || minimized!=lastLoggedMin || maximized!=lastLoggedMax)) {
        StringCchPrintfW(text,512,L"OBSERVE hwnd=%p x=%ld y=%ld width=%ld height=%ld min=%d max=%d want=%dx%d",
          target,r.left,r.top,r.right-r.left,r.bottom-r.top,minimized,maximized,wantedWidth,wantedHeight);
        Log(text); lastLoggedRect=r; lastLoggedMin=minimized; lastLoggedMax=maximized;
        haveLoggedRect=true; lastLogAt=GetTickCount64();
    }
}

static void CheckTarget() {
    queued=false;
    if(!locked || inCorrection) return;
    if(!TargetValid()) { Unlock(L"目标窗口已关闭，固定已自动解除。"); return; }
    RECT r;
    if(!GetWindowRect(target,&r)) { Status(L"暂时无法读取目标窗口，正在等待。 "); return; }
    bool minimized=IsIconic(target)!=FALSE, maximized=IsZoomed(target)!=FALSE;
    Observe(r,minimized,maximized);
    if(minimized) { mismatchSince=0; Status(L"已暂停：窗口最小化后不干预，恢复后继续固定。"); return; }
    if(!IsWindowVisible(target)) { mismatchSince=0; Status(L"窗口已隐藏，显示后继续固定。"); return; }
    bool wrong=maximized || r.right-r.left!=wantedWidth || r.bottom-r.top!=wantedHeight;
    if(!wrong) { mismatchSince=0; Status(L"固定中 · 可自由移动和最小化 · 关闭本程序即解除"); return; }
    ULONGLONG now=GetTickCount64();
    if(!mismatchSince) mismatchSince=now;
    DWORD interval=(now-mismatchSince>3000)?500:60;
    if(lastAttempt && now-lastAttempt<interval) {
        SetTimer(mainWindow,TIMER_SETTLE,interval-static_cast<UINT>(now-lastAttempt),nullptr);
        return;
    }
    lastAttempt=now; inCorrection=true;
    BOOL success;
    if(maximized) {
        Log(L"RESTORE_FROM_MAXIMIZED");
        success=ShowWindowAsync(target,SW_RESTORE);
    } else {
        success=SetWindowPos(target,nullptr,0,0,wantedWidth,wantedHeight,
            SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_ASYNCWINDOWPOS);
        wchar_t text[256]; StringCchPrintfW(text,256,L"CORRECT width=%d height=%d accepted=%d",wantedWidth,wantedHeight,success); Log(text);
    }
    DWORD error=success?0:GetLastError(); inCorrection=false;
    if(!success) {
        wchar_t text[256]; StringCchPrintfW(text,256,L"ERROR win32=%lu",error); Log(text);
        Unlock(L"无法调整目标窗口。请与目标程序使用相同权限后重新选取。"); return;
    }
    if(now-mismatchSince>3000) Status(L"目标程序仍在改变尺寸，或限制了最小大小；正在继续尝试恢复。");
    else Status(L"正在恢复固定尺寸…");
    SetTimer(mainWindow,TIMER_SETTLE,80,nullptr);
}

static bool LockWindow(HWND hwnd, DWORD expectedPid=0) {
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    if(!hwnd || !IsWindow(hwnd) || !pid || (expectedPid && pid!=expectedPid) ||
       pid==GetCurrentProcessId() || !IsWindowVisible(hwnd)) {
        Status(L"这个窗口已关闭或无法选取，请刷新后重试。"); return false;
    }
    if(IsIconic(hwnd)||IsZoomed(hwnd)) { Status(L"请先把目标窗口还原到想要的大小，再选取固定。"); return false; }
    RECT r;
    if(!GetWindowRect(hwnd,&r)||r.right<=r.left||r.bottom<=r.top) {
        Status(L"无法读取窗口尺寸，请重新选取。"); return false;
    }
    Unlock(L""); target=hwnd; targetPid=pid;
    targetProcess=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    wantedWidth=r.right-r.left; wantedHeight=r.bottom-r.top;
    lastAttempt=0; mismatchSince=0; haveLoggedRect=false; destroyed=false;
    locked=true;
    objectHook=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_LOCATIONCHANGE,nullptr,WindowEvent,pid,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    systemHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_MINIMIZEEND,nullptr,WindowEvent,pid,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    wchar_t name[256]={}, text[512]; GetWindowTextW(hwnd,name,256);
    SetText(targetLabel,name[0]?name:L"未命名窗口");
    EnableWindow(unlockButton,TRUE); UpdateTray();
    StringCchPrintfW(text,512,L"LOCK hwnd=%p pid=%lu width=%d height=%d objectHook=%d systemHook=%d",
                    hwnd,pid,wantedWidth,wantedHeight,objectHook!=nullptr,systemHook!=nullptr); Log(text);
    CheckTarget();
    if(!objectHook || !systemHook) Status(L"固定中 · 事件监听不可用，已启用定时恢复");
    return true;
}

static BOOL CALLBACK Enumerate(HWND hwnd, LPARAM) {
    if(candidateCount>=512 || !IsWindowVisible(hwnd)) return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    if(pid==GetCurrentProcessId()) return TRUE;
    LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    if((ex&WS_EX_TOOLWINDOW) && !(ex&WS_EX_APPWINDOW)) return TRUE;
    wchar_t title[256]={}; GetWindowTextW(hwnd,title,256);
    if(!title[0]) return TRUE;
    candidates[candidateCount]={hwnd,pid};
    LRESULT index=SendMessageW(listBox,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    if(index!=LB_ERR && index!=LB_ERRSPACE) SendMessageW(listBox,LB_SETITEMDATA,index,candidateCount++);
    return TRUE;
}

static void RefreshList() {
    HWND selected=nullptr;
    LRESULT old=SendMessageW(listBox,LB_GETCURSEL,0,0);
    if(old!=LB_ERR) { LRESULT data=SendMessageW(listBox,LB_GETITEMDATA,old,0); if(data>=0&&data<candidateCount) selected=candidates[data].hwnd; }
    SendMessageW(listBox,WM_SETREDRAW,FALSE,0);
    SendMessageW(listBox,LB_RESETCONTENT,0,0); candidateCount=0;
    EnumWindows(Enumerate,0);
    for(int i=0;i<candidateCount;++i) if(candidates[i].hwnd==selected) SendMessageW(listBox,LB_SETCURSEL,i,0);
    SendMessageW(listBox,WM_SETREDRAW,TRUE,0); InvalidateRect(listBox,nullptr,TRUE);
    EnableWindow(lockButton,SendMessageW(listBox,LB_GETCURSEL,0,0)!=LB_ERR);
}

static void LockSelected() {
    LRESULT index=SendMessageW(listBox,LB_GETCURSEL,0,0);
    if(index==LB_ERR) { Status(L"请先在列表中选择窗口，或拖动上面的选取按钮。"); return; }
    LRESULT data=SendMessageW(listBox,LB_GETITEMDATA,index,0);
    if(data<0 || data>=candidateCount) { Status(L"窗口列表已变化，请刷新后重新选取。"); return; }
    LockWindow(candidates[data].hwnd,candidates[data].pid);
}

static HWND PickWindow(POINT point) {
    HWND found=GetAncestor(WindowFromPoint(point),GA_ROOT);
    DWORD pid=0; if(found) GetWindowThreadProcessId(found,&pid);
    if(pid!=GetCurrentProcessId()) return found;
    if(!pickerLowered) return nullptr;
    // An active panel may retain its z-order until mouse capture ends. In that
    // case select the first visible window underneath it at the drop point.
    struct Hit { POINT point; HWND hwnd; } hit={point,nullptr};
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL {
        auto h=reinterpret_cast<Hit*>(data);
        DWORD owner=0; GetWindowThreadProcessId(hwnd,&owner);
        if(owner==GetCurrentProcessId() || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        RECT r; if(GetWindowRect(hwnd,&r) && PtInRect(&r,h->point)) {
            h->hwnd=hwnd; return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&hit));
    return hit.hwnd;
}

static LRESULT CALLBACK PickerProcedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                       UINT_PTR, DWORD_PTR) {
    switch(message) {
    case WM_LBUTTONDOWN:
        picking=true; pickerLowered=false;
        pickerStart={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&pickerStart);
        SetFocus(hwnd); SetCapture(hwnd); SendMessageW(hwnd,BM_SETSTATE,TRUE,0);
        SetCursor(LoadCursorW(nullptr,IDC_CROSS));
        Status(L"按住鼠标，把十字拖到目标窗口后松开；Esc 取消。"); return 0;
    case WM_MOUSEMOVE:
        if(picking) {
            SetCursor(LoadCursorW(nullptr,IDC_CROSS));
            POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&p);
            if(!pickerLowered && (p.x-pickerStart.x>8 || pickerStart.x-p.x>8 ||
                                  p.y-pickerStart.y>8 || pickerStart.y-p.y>8)) {
                // Keep mouse capture, but expose windows behind the picker panel.
                pickerLowered=true;
                SetWindowPos(mainWindow,HWND_BOTTOM,0,0,0,0,
                             SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            }
            return 0;
        } break;
    case WM_SETCURSOR:
        if(picking) { SetCursor(LoadCursorW(nullptr,IDC_CROSS)); return TRUE; } break;
    case WM_LBUTTONUP:
        if(picking) {
            // Use the coordinates attached to this release message, even if
            // the pointer has already moved again while the queue is drained.
            POINT point={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&point);
            if(!pickerLowered && (point.x-pickerStart.x>8 || pickerStart.x-point.x>8 ||
                                  point.y-pickerStart.y>8 || pickerStart.y-point.y>8)) pickerLowered=true;
            HWND found=PickWindow(point);
            wchar_t info[256]; StringCchPrintfW(info,256,L"PICK x=%ld y=%ld lowered=%d hwnd=%p",point.x,point.y,pickerLowered,found); Log(info);
            picking=false; ReleaseCapture(); SendMessageW(hwnd,BM_SETSTATE,FALSE,0);
            if(pickerLowered) { pickerLowered=false; ShowMain(); }
            if(found && found!=mainWindow) LockWindow(found);
            else Status(L"未选取：请把选取按钮拖到另一个程序的窗口后松开。");
            return 0;
        } break;
    case WM_CAPTURECHANGED:
        if(picking) {
            picking=false; SendMessageW(hwnd,BM_SETSTATE,FALSE,0);
            if(pickerLowered) { pickerLowered=false; ShowMain(); }
            Status(L"选取已取消。");
        } break;
    case WM_KEYDOWN:
        if(wp==VK_ESCAPE && picking) {
            picking=false; ReleaseCapture(); SendMessageW(hwnd,BM_SETSTATE,FALSE,0);
            if(pickerLowered) { pickerLowered=false; ShowMain(); }
            Status(L"选取已取消。"); return 0;
        } break;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd,PickerProcedure,1); break;
    }
    return DefSubclassProc(hwnd,message,wp,lp);
}

static HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,mainWindow,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE); return h;
}

static void Rounded(HDC dc, RECT r, COLORREF fill, COLORREF outline, int radius) {
    HBRUSH brush=CreateSolidBrush(fill); HPEN pen=CreatePen(PS_SOLID,1,outline);
    HGDIOBJ oldBrush=SelectObject(dc,brush), oldPen=SelectObject(dc,pen);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,Px(radius),Px(radius));
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(brush); DeleteObject(pen);
}

static void PaintBackground(HDC dc) {
    RECT r; GetClientRect(mainWindow,&r); FillRect(dc,&r,backgroundBrush);
    DrawIconEx(dc,Px(24),Px(21),appIcon,Px(24),Px(24),0,nullptr,DI_NORMAL);
    Rounded(dc,{Px(24),Px(92),r.right-Px(24),Px(224)},Card,Border,14);
    Rounded(dc,{Px(24),Px(332),r.right-Px(24),Px(466)},Card,Border,12);
    COLORREF fill=badgeState==BadgeState::Active?RGB(220,252,231):
        badgeState==BadgeState::Paused?RGB(254,243,199):
        badgeState==BadgeState::Picking?RGB(219,234,254):RGB(229,231,235);
    Rounded(dc,BadgeRect(),fill,fill,20);
    HPEN pen=CreatePen(PS_SOLID,1,Border); HGDIOBJ old=SelectObject(dc,pen);
    MoveToEx(dc,Px(24),Px(570),nullptr); LineTo(dc,r.right-Px(24),Px(570));
    SelectObject(dc,old); DeleteObject(pen);
}

static void DrawButton(const DRAWITEMSTRUCT& item) {
    bool enabled=(item.itemState&ODS_DISABLED)==0, pressed=(item.itemState&ODS_SELECTED)!=0;
    bool hover=item.hwndItem==hoveredButton;
    bool primary=item.CtlID==ID_PICK;
    bool quiet=item.CtlID==ID_REFRESH||item.CtlID==ID_EXIT;
    COLORREF fill=primary?Accent:quiet?Background:Card, edge=quiet?Background:Border;
    COLORREF ink=primary?Card:Ink;
    if(item.CtlID==ID_LOCK) { fill=Selection; edge=RGB(191,219,254); ink=Accent; }
    if(quiet) ink=Muted;
    if(hover||pressed) {
        fill=primary?(pressed?RGB(30,64,175):RGB(29,78,216)):
            item.CtlID==ID_LOCK?RGB(219,234,254):RGB(229,231,235);
        edge=fill;
    }
    if(!enabled) { fill=RGB(243,244,246); edge=fill; ink=RGB(156,163,175); }
    HDC dc=item.hDC; SetBkMode(dc,TRANSPARENT);
    Rounded(dc,item.rcItem,fill,primary?fill:edge,10);
    SetTextColor(dc,ink); HGDIOBJ old=SelectObject(dc,font);
    RECT textRect=item.rcItem; InflateRect(&textRect,-Px(12),0);
    wchar_t text[256]; GetWindowTextW(item.hwndItem,text,256);
    DrawTextW(dc,text,-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    if((item.itemState&ODS_FOCUS)&&!(item.itemState&ODS_NOFOCUSRECT)) {
        RECT focus=item.rcItem; InflateRect(&focus,-Px(4),-Px(4)); DrawFocusRect(dc,&focus);
    }
    SelectObject(dc,old);
}

static void DrawListRow(const DRAWITEMSTRUCT& item) {
    HDC dc=item.hDC; bool selected=(item.itemState&ODS_SELECTED)!=0;
    HBRUSH brush=selected?CreateSolidBrush(Selection):cardBrush;
    FillRect(dc,&item.rcItem,brush); if(selected) DeleteObject(brush);
    if(item.itemID==static_cast<UINT>(-1)) return;
    wchar_t title[256]={}; SendMessageW(listBox,LB_GETTEXT,item.itemID,reinterpret_cast<LPARAM>(title));
    RECT text=item.rcItem; text.left+=Px(14); text.right-=Px(12);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,selected?RGB(29,78,216):Ink);
    HGDIOBJ old=SelectObject(dc,font);
    DrawTextW(dc,title,-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(dc,old);
    if(selected) {
        RECT stripe=item.rcItem; stripe.right=stripe.left+Px(3);
        HBRUSH accentBrush=CreateSolidBrush(Accent); FillRect(dc,&stripe,accentBrush); DeleteObject(accentBrush);
    }
    if((item.itemState&ODS_FOCUS)&&!(item.itemState&ODS_NOFOCUSRECT)) {
        RECT focus=item.rcItem; InflateRect(&focus,-Px(3),-Px(3)); DrawFocusRect(dc,&focus);
    }
}

static LRESULT CALLBACK ButtonProcedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                       UINT_PTR, DWORD_PTR) {
    if(message==WM_MOUSEMOVE && hoveredButton!=hwnd) {
        HWND old=hoveredButton; hoveredButton=hwnd;
        TRACKMOUSEEVENT track={sizeof track,TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
        if(old) InvalidateRect(old,nullptr,FALSE); InvalidateRect(hwnd,nullptr,FALSE);
    } else if(message==WM_MOUSELEAVE) {
        if(hoveredButton==hwnd) hoveredButton=nullptr; InvalidateRect(hwnd,nullptr,FALSE);
    } else if(message==WM_NCDESTROY) {
        if(hoveredButton==hwnd) hoveredButton=nullptr; RemoveWindowSubclass(hwnd,ButtonProcedure,2);
    }
    return DefSubclassProc(hwnd,message,wp,lp);
}

static void Layout() {
    RECT r; GetClientRect(mainWindow,&r); int w=r.right, m=Px(24), gap=Px(10), full=w-2*m;
    MoveWindow(brandLabel,Px(58),Px(17),w-Px(174),Px(32),TRUE);
    MoveWindow(hintLabel,m,Px(56),full,Px(20),TRUE);
    RECT badge=BadgeRect(); MoveWindow(badgeLabel,badge.left,badge.top+Px(4),badge.right-badge.left,Px(20),TRUE);
    MoveWindow(targetCaption,Px(40),Px(107),full-Px(32),Px(18),TRUE);
    MoveWindow(targetLabel,Px(40),Px(130),full-Px(32),Px(28),TRUE);
    int column=(full-Px(32))/2;
    MoveWindow(wantedCaption,Px(40),Px(167),column,Px(18),TRUE);
    MoveWindow(currentCaption,Px(40)+column,Px(167),column,Px(18),TRUE);
    MoveWindow(sizeLabel,Px(40),Px(188),column,Px(27),TRUE);
    MoveWindow(currentSizeLabel,Px(40)+column,Px(188),column,Px(27),TRUE);
    MoveWindow(pickerButton,m,Px(240),full,Px(48),TRUE);
    MoveWindow(listCaption,m,Px(307),full-Px(120),Px(20),TRUE);
    MoveWindow(refreshButton,w-m-Px(112),Px(298),Px(112),Px(30),TRUE);
    MoveWindow(listBox,m+Px(4),Px(337),full-Px(8),Px(124),TRUE);
    MoveWindow(lockButton,m,Px(480),full,Px(38),TRUE);
    int third=(full-2*gap)/3;
    MoveWindow(unlockButton,m,Px(532),third,Px(38),TRUE);
    MoveWindow(hideButton,m+third+gap,Px(532),third,Px(38),TRUE);
    MoveWindow(exitButton,m+2*(third+gap),Px(532),full-2*(third+gap),Px(38),TRUE);
    MoveWindow(statusLabel,m,Px(589),full,Px(38),TRUE);
    MoveWindow(shortcutLabel,m,Px(638),full,Px(18),TRUE);
    SendMessageW(listBox,LB_SETITEMHEIGHT,0,Px(31)); InvalidateRect(mainWindow,nullptr,FALSE);
}

static void RecreateFonts() {
    HFONT old=font, oldTitle=titleFont, oldTarget=targetFont, oldMetric=metricFont, oldSmall=smallFont;
    font=CreateFontW(-Px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    titleFont=CreateFontW(-Px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    targetFont=CreateFontW(-Px(17),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    metricFont=CreateFontW(-Px(20),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    smallFont=CreateFontW(-Px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HWND controls[]={listBox,statusLabel,lockButton,unlockButton,pickerButton,refreshButton,hideButton,exitButton,listCaption};
    for(HWND h:controls) if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    HWND small[]={hintLabel,badgeLabel,targetCaption,wantedCaption,currentCaption,shortcutLabel};
    for(HWND h:small) if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
    if(brandLabel) SendMessageW(brandLabel,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
    if(targetLabel) SendMessageW(targetLabel,WM_SETFONT,reinterpret_cast<WPARAM>(targetFont),TRUE);
    if(sizeLabel) SendMessageW(sizeLabel,WM_SETFONT,reinterpret_cast<WPARAM>(metricFont),TRUE);
    if(currentSizeLabel) SendMessageW(currentSizeLabel,WM_SETFONT,reinterpret_cast<WPARAM>(metricFont),TRUE);
    if(old) DeleteObject(old); if(oldTitle) DeleteObject(oldTitle); if(oldTarget) DeleteObject(oldTarget);
    if(oldMetric) DeleteObject(oldMetric); if(oldSmall) DeleteObject(oldSmall);
}

static void TrayMenu() {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,ID_TRAY_OPEN,L"打开窗口尺寸固定");
    AppendMenuW(menu,MF_STRING|(locked?0:MF_GRAYED),ID_TRAY_UNLOCK,L"解除固定");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,ID_TRAY_EXIT,L"退出并解除固定");
    POINT p; GetCursorPos(&p); SetForegroundWindow(mainWindow);
    UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,mainWindow,nullptr);
    DestroyMenu(menu); PostMessageW(mainWindow,WM_NULL,0,0);
    if(command==ID_TRAY_OPEN) ShowMain();
    else if(command==ID_TRAY_UNLOCK) Unlock(L"已解除固定，可正常调整窗口大小。");
    else if(command==ID_TRAY_EXIT) DestroyWindow(mainWindow);
}

static LRESULT CALLBACK MainProcedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if(message==taskbarCreated) { trayAdded=false; AddTray(); return 0; }
    switch(message) {
    case WM_CREATE: {
        mainWindow=hwnd; if(dpiForWindow) dpi=static_cast<int>(dpiForWindow(hwnd)); RecreateFonts();
        brandLabel=Control(L"STATIC",L"窗口尺寸固定",SS_LEFT,0);
        hintLabel=Control(L"STATIC",L"先调整目标窗口大小，再选取。无需填写数字。",SS_LEFT,0);
        badgeLabel=Control(L"STATIC",L"未固定",SS_CENTER,0);
        targetCaption=Control(L"STATIC",L"目标窗口",SS_LEFT,0);
        targetLabel=Control(L"STATIC",L"还没有选取窗口",SS_LEFT|SS_ENDELLIPSIS,0);
        wantedCaption=Control(L"STATIC",L"固定尺寸 · px",SS_LEFT,0);
        currentCaption=Control(L"STATIC",L"当前尺寸 · px",SS_LEFT,0);
        sizeLabel=Control(L"STATIC",L"— × —",SS_LEFT,0);
        currentSizeLabel=Control(L"STATIC",L"— × —",SS_LEFT,0);
        pickerButton=Control(L"BUTTON",L"按住拖到目标窗口 · 松开即可固定",WS_TABSTOP|BS_OWNERDRAW,ID_PICK);
        listCaption=Control(L"STATIC",L"或从窗口列表选择",SS_LEFT,0);
        listBox=Control(L"LISTBOX",L"窗口列表",WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,ID_LIST);
        lockButton=Control(L"BUTTON",L"固定选中窗口",WS_TABSTOP|BS_OWNERDRAW,ID_LOCK);
        refreshButton=Control(L"BUTTON",L"刷新列表",WS_TABSTOP|BS_OWNERDRAW,ID_REFRESH);
        unlockButton=Control(L"BUTTON",L"解除固定",WS_TABSTOP|BS_OWNERDRAW,ID_UNLOCK);
        hideButton=Control(L"BUTTON",L"收起到托盘",WS_TABSTOP|BS_OWNERDRAW,ID_HIDE);
        exitButton=Control(L"BUTTON",L"退出",WS_TABSTOP|BS_OWNERDRAW,ID_EXIT);
        statusLabel=Control(L"STATIC",L"拖动选取或双击列表，即可固定目标窗口的当前大小。",SS_LEFT,0);
        shortcutLabel=Control(L"STATIC",L"Ctrl + Alt + Z 固定前台窗口   ·   Ctrl + Alt + U 解除",SS_LEFT,0);
        RecreateFonts();
        HWND buttons[]={pickerButton,lockButton,refreshButton,unlockButton,hideButton,exitButton};
        for(HWND button:buttons) SetWindowSubclass(button,ButtonProcedure,2,0);
        SetWindowSubclass(pickerButton,PickerProcedure,1,0);
        EnableWindow(unlockButton,FALSE); RefreshList(); Layout();
        captureHotkey=RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'Z')!=FALSE;
        unlockHotkey=RegisterHotKey(hwnd,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'U')!=FALSE;
        if(!captureHotkey||!unlockHotkey) Status(L"部分快捷键被其他程序占用；请使用拖动选取或窗口列表。");
        SetTimer(hwnd,TIMER_WATCH,1000,nullptr); AddTray(); return 0;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case ID_LOCK: LockSelected(); break;
        case ID_LIST:
            if(HIWORD(wp)==LBN_DBLCLK) LockSelected();
            else if(HIWORD(wp)==LBN_SELCHANGE) EnableWindow(lockButton,SendMessageW(listBox,LB_GETCURSEL,0,0)!=LB_ERR);
            break;
        case ID_UNLOCK: Unlock(L"已解除固定，可正常调整窗口大小。"); break;
        case ID_REFRESH: RefreshList(); if(!locked) Status(L"列表已刷新。选取后点击固定，或双击目标窗口。"); break;
        case ID_PICK: Status(L"请按住此按钮，拖到目标窗口后松开；也可从下方列表选取。"); break;
        case ID_HIDE: if(trayAdded) ShowWindow(hwnd,SW_HIDE); else Status(L"托盘暂不可用，请使用最小化按钮。"); break;
        case ID_EXIT: DestroyWindow(hwnd); break;
        } return 0;
    case WM_CHECK: CheckTarget(); return 0;
    case WM_HOTKEY:
        if(wp==1) LockWindow(GetForegroundWindow());
        else if(wp==2) Unlock(L"已解除固定，可正常调整窗口大小。"); return 0;
    case WM_SETFOCUS: if(listBox) SetFocus(listBox); return 0;
    case WM_TIMER:
        if(wp==TIMER_SETTLE) KillTimer(hwnd,TIMER_SETTLE);
        if(wp==TIMER_WATCH||wp==TIMER_SETTLE) CheckTarget(); return 0;
    case WM_TRAY:
        if(lp==WM_LBUTTONUP||lp==WM_LBUTTONDBLCLK) ShowMain();
        else if(lp==WM_RBUTTONUP||lp==WM_CONTEXTMENU) TrayMenu(); return 0;
    case WM_SIZE: if(wp!=SIZE_MINIMIZED) Layout(); return 0;
    case WM_DPICHANGED: {
        dpi=HIWORD(wp); RecreateFonts(); RECT* suggested=reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
        Layout(); return 0;
    }
    case WM_MEASUREITEM: {
        auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if(item->CtlID==ID_LIST) { item->itemHeight=Px(31); return TRUE; } break;
    }
    case WM_DRAWITEM: {
        auto item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(item->CtlID==ID_LIST) DrawListRow(*item); else DrawButton(*item);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc=BeginPaint(hwnd,&paint); PaintBackground(dc); EndPaint(hwnd,&paint); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wp); HWND label=reinterpret_cast<HWND>(lp);
        SetBkMode(dc,TRANSPARENT);
        bool inCard=label==targetLabel||label==sizeLabel||label==currentSizeLabel||label==targetCaption||label==wantedCaption||label==currentCaption;
        COLORREF ink=label==brandLabel||label==targetLabel||label==sizeLabel?Ink:label==currentSizeLabel?Accent:Muted;
        if(label==badgeLabel) ink=badgeState==BadgeState::Active?RGB(21,128,61):badgeState==BadgeState::Paused?RGB(146,64,14):badgeState==BadgeState::Picking?Accent:Muted;
        SetTextColor(dc,ink);
        return reinterpret_cast<LRESULT>(label==badgeLabel?GetStockObject(NULL_BRUSH):inCard?cardBrush:backgroundBrush);
    }
    case WM_CTLCOLORLISTBOX: SetBkColor(reinterpret_cast<HDC>(wp),Card); return reinterpret_cast<LRESULT>(cardBrush);
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        Unlock(L""); KillTimer(hwnd,TIMER_WATCH);
        if(captureHotkey) UnregisterHotKey(hwnd,1); if(unlockHotkey) UnregisterHotKey(hwnd,2);
        if(trayAdded) { NOTIFYICONDATAW n={sizeof n}; n.hWnd=hwnd; n.uID=1; Shell_NotifyIconW(NIM_DELETE,&n); }
        if(font) DeleteObject(font); if(titleFont) DeleteObject(titleFont);
        if(targetFont) DeleteObject(targetFont); if(metricFont) DeleteObject(metricFont); if(smallFont) DeleteObject(smallFont);
        Log(L"EXIT"); if(logFile!=INVALID_HANDLE_VALUE) CloseHandle(logFile);
        PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int show) {
    instance=app;
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\WindowSizeLock.Native.1");
    if(mutex && GetLastError()==ERROR_ALREADY_EXISTS) {
        HWND existing=FindWindowW(L"WindowSizeLock.Native",nullptr);
        if(existing) { ShowWindow(existing,SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(mutex); return 0;
    }
    InitializeDpi();
    INITCOMMONCONTROLSEX cc={sizeof cc,ICC_STANDARD_CLASSES}; InitCommonControlsEx(&cc);
    StartDiagnostics(); taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    appIcon=LoadIconW(app,MAKEINTRESOURCEW(1)); if(!appIcon) appIcon=LoadIconW(nullptr,IDI_APPLICATION);
    WNDCLASSEXW cls={sizeof cls}; cls.hInstance=app; cls.lpfnWndProc=MainProcedure;
    cls.lpszClassName=L"WindowSizeLock.Native"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    backgroundBrush=CreateSolidBrush(Background); cardBrush=CreateSolidBrush(Card);
    cls.hbrBackground=backgroundBrush; cls.hIcon=cls.hIconSm=appIcon;
    if(!RegisterClassExW(&cls)) return 1;
    HDC screen=GetDC(nullptr); dpi=GetDeviceCaps(screen,LOGPIXELSX); ReleaseDC(nullptr,screen);
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT r={0,0,Px(580),Px(676)};
    if(adjustForDpi) adjustForDpi(&r,style,FALSE,0,dpi); else AdjustWindowRectEx(&r,style,FALSE,0);
    HWND hwnd=CreateWindowExW(0,cls.lpszClassName,L"窗口尺寸固定",style,CW_USEDEFAULT,CW_USEDEFAULT,
                              r.right-r.left,r.bottom-r.top,nullptr,nullptr,app,nullptr);
    if(!hwnd) { if(mutex) CloseHandle(mutex); return 2; }
    ShowWindow(hwnd,show); UpdateWindow(hwnd);
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0) {
        if(!IsDialogMessageW(hwnd,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if(mutex) CloseHandle(mutex);
    DeleteObject(backgroundBrush); DeleteObject(cardBrush);
    return 0;
}
