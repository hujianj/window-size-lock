#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <dwmapi.h>
#include <cstddef>
#include <cwchar>

// All state and WinEvent callbacks belong to the main message-loop thread.
enum { ID_LIST=101, ID_LOCK, ID_UNLOCK, ID_REFRESH, ID_PICK, ID_HIDE, ID_EXIT,
       ID_MANAGED, ID_ENABLE, ID_PAUSE, ID_ALL_ENABLE, ID_ALL_PAUSE,
       ID_WIDTH, ID_HEIGHT, ID_APPLY, ID_MAXSIZE,
       ID_TRAY_OPEN=201, ID_TRAY_UNLOCK, ID_TRAY_EXIT };
constexpr UINT WM_CHECK=WM_APP+1, WM_TRAY=WM_APP+2;
constexpr UINT_PTR TIMER_WATCH=1, TIMER_SETTLE=2;
constexpr int MaxTargets=32;
struct Candidate { HWND hwnd; DWORD pid; };
struct Target {
    bool saved{};
    wchar_t executable[1024]{},windowClass[256]{},title[256]{};
    HWND hwnd{}; DWORD pid{}; HANDLE process{};
    int width{},height{},remaxAttempts{};
    bool enabled{},destroyed{},moving{},movingTitle{},captureMax{},haveLog{},logMin{},logMax{},screenSized{};
    bool haveGeometry{},geometryMin{},geometryMax{},remaxPending{};
    HWINEVENTHOOK objectHook{},systemHook{};
    ULONGLONG lastAttempt{},mismatchSince{},settleUntil{},captureUntil{},geometrySince{},remaxUntil{};
    RECT logRect{},geometry{},moveStart{};
    wchar_t row[512]{};
};
static Target targets[MaxTargets];
static HINSTANCE instance;
static HWND mainWindow,listBox,managedBox,targetLabel,sizeLabel,statusLabel,lockButton,
    unlockButton,pickerButton,refreshButton,hideButton,exitButton;
static HWND brandLabel,hintLabel,badgeLabel,targetCaption,wantedCaption,currentCaption,
    currentSizeLabel,listCaption,shortcutLabel,managedCaption,enableButton,pauseButton,
    allEnableButton,allPauseButton,widthEdit,heightEdit,applyButton,maxSizeButton,
    widthCaption,heightCaption,visibleLabel,screenLabel,workLabel,modeLabel,metricNote;
static HFONT font,titleFont,targetFont,metricFont,smallFont;
static HBRUSH backgroundBrush,cardBrush;
static constexpr COLORREF Background=RGB(245,247,251),Card=RGB(255,255,255),
    Ink=RGB(31,41,55),Muted=RGB(107,114,128),Accent=RGB(37,99,235),
    Border=RGB(226,232,240),Selection=RGB(239,246,255);
enum class BadgeState { Ready,Active,Paused,Picking };
static BadgeState badgeState=BadgeState::Ready;
static HWND hoveredButton,detailWindow;
static int detailSlot=-1;
static wchar_t settingsPath[1200]{};
static bool settingsDirty;
static ULONGLONG saveAttempt;
static Candidate candidates[512];
static int candidateCount;
static bool queued,picking,trayAdded;
static int trayActive=-1,trayTotal=-1;
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
    // Local test opt-in. Normal use creates settings but no diagnostics log.
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
    Log(L"START version=1.3.0");
}

static RECT BadgeRect() {
    RECT r; GetClientRect(mainWindow,&r);
    return {r.right-Px(154),Px(22),r.right-Px(24),Px(48)};
}
static int FindTarget(HWND h) {
    for(int i=0;i<MaxTargets;++i) if(targets[i].hwnd==h && h) return i;
    return -1;
}
static int CountTargets(bool enabledOnly=false) {
    int n=0; for(const auto& t:targets) if(t.saved && (!enabledOnly||t.enabled)) ++n;
    return n;
}
static void UpdateBadge() {
    int total=CountTargets(),active=CountTargets(true);
    BadgeState next=picking?BadgeState::Picking:!total?BadgeState::Ready:
        active?BadgeState::Active:BadgeState::Paused;
    wchar_t text[64];
    if(picking) StringCchCopyW(text,64,L"选取中");
    else StringCchPrintfW(text,64,L"启用 %d / %d",active,total);
    wchar_t previous[64]={}; GetWindowTextW(badgeLabel,previous,64);
    bool changed=lstrcmpW(previous,text)!=0;
    SetText(badgeLabel,text);
    // The transparent static needs its parent to repaint even when only the
    // count changes and the badge keeps the same active/paused color.
    if(changed||next!=badgeState) { badgeState=next; RECT r=BadgeRect(); InvalidateRect(mainWindow,&r,FALSE); InvalidateRect(badgeLabel,nullptr,TRUE); }
}
static void Status(const wchar_t* message) { SetText(statusLabel,message); UpdateBadge(); }

// Explicit, bounded disk format: no HWND, PID, pointers or runtime state.
struct SavedRecord {
    DWORD width,height,flags;
    wchar_t executable[1024],windowClass[256],title[256];
};
struct SavedFile { DWORD magic,version,count; SavedRecord records[MaxTargets]; };
static SavedFile settingsBuffer;
static constexpr DWORD SettingsMagic=0x4B4C5357;
static void InitializeSettings() {
    wchar_t folder[1100];
    DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",folder,1024);
    if(!n||n>=1024) return;
    if(FAILED(StringCchCatW(folder,1100,L"\\WindowSizeLock"))) return;
    if(!CreateDirectoryW(folder,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS) return;
    StringCchPrintfW(settingsPath,1200,L"%s\\settings.dat",folder);
}
static bool SaveSettings() {
    settingsDirty=true; saveAttempt=GetTickCount64();
    auto& file=settingsBuffer; file={}; file.magic=SettingsMagic; file.version=1;
    for(const auto& t:targets) if(t.saved) {
        auto& r=file.records[file.count++];
        r.width=t.width; r.height=t.height; r.flags=(t.enabled?1:0)|(t.screenSized?2:0);
        StringCchCopyW(r.executable,1024,t.executable); StringCchCopyW(r.windowClass,256,t.windowClass); StringCchCopyW(r.title,256,t.title);
    }
    wchar_t temporary[1240]; StringCchPrintfW(temporary,1240,L"%s.tmp",settingsPath);
    HANDLE h=settingsPath[0]?CreateFileW(temporary,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr):INVALID_HANDLE_VALUE;
    bool ok=false;
    if(h!=INVALID_HANDLE_VALUE) {
        DWORD bytes=static_cast<DWORD>(offsetof(SavedFile,records)+file.count*sizeof(SavedRecord)),written=0;
        ok=WriteFile(h,&file,bytes,&written,nullptr)&&written==bytes&&FlushFileBuffers(h);
        CloseHandle(h);
        if(ok) ok=MoveFileExW(temporary,settingsPath,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    }
    if(ok) settingsDirty=false;
    else if(IsWindow(statusLabel)) Status(L"配置暂未写入成功，请检查本地配置目录的空间或写入权限。将自动重试。");
    return ok;
}
static void LoadSettings() {
    if(!settingsPath[0]) { Status(L"无法访问本地配置目录，本次记录暂不能保存。"); return; }
    HANDLE h=CreateFileW(settingsPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) { if(GetLastError()!=ERROR_FILE_NOT_FOUND) Status(L"配置无法读取，请检查本地目录权限。"); return; }
    auto& file=settingsBuffer; file={}; DWORD got=0;
    LARGE_INTEGER size={};
    bool ok=GetFileSizeEx(h,&size)&&size.QuadPart>=offsetof(SavedFile,records)&&size.QuadPart<=sizeof file;
    if(ok) ok=ReadFile(h,&file,sizeof file,&got,nullptr)!=FALSE;
    CloseHandle(h);
    ok=ok&&file.magic==SettingsMagic&&file.version==1&&file.count<=MaxTargets&&
        got==offsetof(SavedFile,records)+file.count*sizeof(SavedRecord);
    for(DWORD k=0;ok&&k<file.count;++k) {
        const auto& r=file.records[k];
        ok=r.width>=1&&r.width<=32767&&r.height>=1&&r.height<=32767&&r.flags<=3&&
            r.executable[0]&&r.windowClass[0]&&r.title[0]&&
            wmemchr(r.executable,0,1024)&&wmemchr(r.windowClass,0,256)&&wmemchr(r.title,0,256);
    }
    if(!ok) { Status(L"配置文件损坏或版本不支持，未载入；原文件保留，重新选取将创建新配置。"); return; }
    for(DWORD k=0;k<file.count;++k) {
        const auto& r=file.records[k]; auto& t=targets[k]; t.saved=true;
        t.width=r.width; t.height=r.height; t.enabled=(r.flags&1)!=0; t.screenSized=(r.flags&2)!=0;
        StringCchCopyW(t.executable,1024,r.executable); StringCchCopyW(t.windowClass,256,r.windowClass); StringCchCopyW(t.title,256,r.title);
    }
    if(file.count) Status(L"已读取保存的固定记录，等待相同程序、窗口类型和标题的窗口。");
}
static void NormalizedTitle(HWND h,wchar_t* title) {
    wchar_t raw[256]={}; GetWindowTextW(h,raw,256);
    // Classic Notepad prefixes its title with '*' while the document is dirty.
    const wchar_t* p=raw; if(*p==L'*') ++p;
    StringCchCopyW(title,256,p);
}
static bool ReadIdentity(HWND h,Target& identity) {
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    DWORD length=1024;
    bool ok=process&&QueryFullProcessImageNameW(process,0,identity.executable,&length);
    if(process) CloseHandle(process);
    if(!ok||!GetClassNameW(h,identity.windowClass,256)) return false;
    NormalizedTitle(h,identity.title); return identity.title[0]!=0;
}
static bool SameIdentity(const Target& a,const Target& b) {
    return lstrcmpiW(a.executable,b.executable)==0&&lstrcmpW(a.windowClass,b.windowClass)==0&&lstrcmpW(a.title,b.title)==0;
}
static int DetailIndex() {
    return detailSlot>=0&&detailSlot<MaxTargets&&targets[detailSlot].saved?detailSlot:FindTarget(detailWindow);
}
static void UpdateTray() {
    if(!trayAdded) return;
    int active=CountTargets(true),total=CountTargets();
    if(active==trayActive && total==trayTotal) return;
    NOTIFYICONDATAW n={sizeof n}; n.hWnd=mainWindow; n.uID=1; n.uFlags=NIF_TIP;
    StringCchPrintfW(n.szTip,128,L"窗口尺寸固定：启用 %d / %d 个窗口",active,total);
    if(Shell_NotifyIconW(NIM_MODIFY,&n)) { trayActive=active; trayTotal=total; }
}
static void AddTray() {
    NOTIFYICONDATAW n={sizeof n}; n.hWnd=mainWindow; n.uID=1;
    n.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; n.hIcon=appIcon; n.uCallbackMessage=WM_TRAY;
    StringCchCopyW(n.szTip,128,L"窗口尺寸固定"); trayAdded=Shell_NotifyIconW(NIM_ADD,&n)!=FALSE;
    trayActive=trayTotal=-1; UpdateTray();
}
static void ShowMain() {
    ShowWindow(mainWindow,SW_RESTORE);
    SetWindowPos(mainWindow,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    SetForegroundWindow(mainWindow);
}
static bool TargetValid(const Target& t) {
    if(t.destroyed||!t.hwnd||!IsWindow(t.hwnd)) return false;
    DWORD pid=0; GetWindowThreadProcessId(t.hwnd,&pid);
    return pid==t.pid && (!t.process||WaitForSingleObject(t.process,0)!=WAIT_OBJECT_0);
}
static const wchar_t* TargetState(const Target& t) {
    if(!t.hwnd) return t.enabled?L"等待窗口":L"暂停·等待";
    if(!t.enabled) return L"已暂停";
    if(IsIconic(t.hwnd)) return L"最小化";
    if(!IsWindowVisible(t.hwnd)) return L"已隐藏";
    if(t.captureMax) return L"读取中";
    if(t.moving) return L"移动中";
    if(IsZoomed(t.hwnd)) return L"最大化";
    return L"固定中";
}
static MONITORINFOEXW MonitorInfo(HWND h) {
    MONITORINFOEXW info={}; info.cbSize=sizeof info;
    GetMonitorInfoW(MonitorFromWindow(h?h:mainWindow,MONITOR_DEFAULTTONEAREST),&info);
    return info;
}
static bool ScreenDimensions(const Target& t) {
    auto m=MonitorInfo(t.hwnd);
    UINT targetDpi=dpiForWindow?dpiForWindow(t.hwnd):96;
    using MetricsFunction=int (WINAPI*)(int,UINT);
    static auto metrics=reinterpret_cast<MetricsFunction>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetSystemMetricsForDpi"));
    int borderX=metrics?metrics(SM_CXSIZEFRAME,targetDpi)+metrics(SM_CXPADDEDBORDER,targetDpi):GetSystemMetrics(SM_CXSIZEFRAME)+GetSystemMetrics(SM_CXPADDEDBORDER);
    int borderY=metrics?metrics(SM_CYSIZEFRAME,targetDpi)+metrics(SM_CXPADDEDBORDER,targetDpi):GetSystemMetrics(SM_CYSIZEFRAME)+GetSystemMetrics(SM_CXPADDEDBORDER);
    auto matches=[&](const RECT& r) {
        int w=r.right-r.left,h=r.bottom-r.top;
        return (t.width==w && t.height==h) ||
            (t.width==w+2*borderX && t.height==h+2*borderY);
    };
    return matches(m.rcWork)||matches(m.rcMonitor);
}
static bool MaxSizeMatches(const Target& t,const RECT& r) {
    // A captured maximized size is a mode, not just a rectangle. Never undo
    // that mode because a frame/DPI/animation changed a few pixels.
    if(t.screenSized||ScreenDimensions(t)) return true;
    if(r.right-r.left==t.width && r.bottom-r.top==t.height) return true;
    return false;
}
static bool FillsWorkArea(HWND h,const RECT& outer) {
    auto m=MonitorInfo(h); RECT visible={};
    if(FAILED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof visible))) visible=outer;
    // Covers the work area, allowing the system's invisible frame and rounding.
    return visible.left<=m.rcWork.left+2 && visible.top<=m.rcWork.top+2 &&
        visible.right>=m.rcWork.right-2 && visible.bottom>=m.rcWork.bottom-2;
}
static void FillInputs() {
    int i=DetailIndex(); RECT r={};
    bool ok=detailWindow && IsWindow(detailWindow) && !IsIconic(detailWindow) && GetWindowRect(detailWindow,&r);
    wchar_t text[32];
    int w=i>=0?targets[i].width:ok?static_cast<int>(r.right-r.left):0;
    int h=i>=0?targets[i].height:ok?static_cast<int>(r.bottom-r.top):0;
    if(w) StringCchPrintfW(text,32,L"%d",w); else text[0]=0; SetText(widthEdit,text);
    if(h) StringCchPrintfW(text,32,L"%d",h); else text[0]=0; SetText(heightEdit,text);
}
static void UpdateDetail(bool fillInputs=false) {
    int i=DetailIndex(); HWND h=i>=0?targets[i].hwnd:detailWindow;
    bool valid=h && IsWindow(h);
    wchar_t text[512],name[256]={};
    if(valid) GetWindowTextW(h,name,256);
    SetText(targetLabel,valid?(name[0]?name:L"未命名窗口"):i>=0?targets[i].title:L"选择窗口，查看尺寸或修改");
    RECT r={}; bool got=valid && !IsIconic(h) && GetWindowRect(h,&r);
    if(got) StringCchPrintfW(text,512,L"%ld × %ld",r.right-r.left,r.bottom-r.top);
    else StringCchCopyW(text,512,valid?L"已最小化":i>=0?L"窗口未打开":L"— × —");
    SetText(currentSizeLabel,text);
    RECT visible={}; bool dwm=got && SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof visible));
    if(dwm) StringCchPrintfW(text,512,L"可见外框：%ld × %ld px",visible.right-visible.left,visible.bottom-visible.top);
    else StringCchCopyW(text,512,L"可见外框：—"); SetText(visibleLabel,text);
    if(i>=0) StringCchPrintfW(text,512,L"%d × %d",targets[i].width,targets[i].height);
    else StringCchCopyW(text,512,L"尚未固定"); SetText(sizeLabel,text);
    auto m=MonitorInfo(valid?h:mainWindow);
    StringCchPrintfW(text,512,L"屏幕分辨率：%ld × %ld px",m.rcMonitor.right-m.rcMonitor.left,m.rcMonitor.bottom-m.rcMonitor.top); SetText(screenLabel,text);
    StringCchPrintfW(text,512,L"最大化工作区：%ld × %ld px",m.rcWork.right-m.rcWork.left,m.rcWork.bottom-m.rcWork.top); SetText(workLabel,text);
    if(i>=0) {
        const auto& t=targets[i];
        StringCchPrintfW(text,512,L"%s · %s",TargetState(t),t.screenSized?L"屏幕尺寸，允许最大化":L"按设定宽高固定");
    } else StringCchCopyW(text,512,valid?L"仅查看 · 点击应用尺寸即可加入固定":L"未选择 · 当前显示本面板所在屏幕");
    SetText(modeLabel,text);
    EnableWindow(widthEdit,valid||i>=0); EnableWindow(heightEdit,valid||i>=0);
    EnableWindow(applyButton,valid||i>=0); EnableWindow(maxSizeButton,valid);
    if(fillInputs) FillInputs();
}
static void UpdateManaged() {
    bool changed=false; int n=0;
    for(auto& t:targets) if(t.saved) {
        wchar_t title[256]={},row[512];
        if(t.hwnd) GetWindowTextW(t.hwnd,title,256); else StringCchCopyW(title,256,t.title);
        StringCchPrintfW(row,512,L"[%s] %s  ·  %d × %d",TargetState(t),title[0]?title:L"未命名窗口",t.width,t.height);
        if(lstrcmpW(t.row,row)!=0) { StringCchCopyW(t.row,512,row); changed=true; } ++n;
    }
    if(SendMessageW(managedBox,LB_GETCOUNT,0,0)!=n) changed=true;
    if(changed) {
        bool selected[MaxTargets]={}; int indices[MaxTargets];
        LRESULT count=SendMessageW(managedBox,LB_GETSELITEMS,MaxTargets,reinterpret_cast<LPARAM>(indices));
        for(int k=0;k<count;++k) { LRESULT slot=SendMessageW(managedBox,LB_GETITEMDATA,indices[k],0); if(slot>=0&&slot<MaxTargets) selected[slot]=true; }
        SendMessageW(managedBox,WM_SETREDRAW,FALSE,0); SendMessageW(managedBox,LB_RESETCONTENT,0,0);
        for(int k=0;k<MaxTargets;++k) if(targets[k].saved) {
            LRESULT row=SendMessageW(managedBox,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(targets[k].row));
            SendMessageW(managedBox,LB_SETITEMDATA,row,k);
            if(selected[k]) SendMessageW(managedBox,LB_SETSEL,TRUE,row);
        }
        SendMessageW(managedBox,WM_SETREDRAW,TRUE,0); InvalidateRect(managedBox,nullptr,TRUE);
    }
    bool any=SendMessageW(managedBox,LB_GETSELCOUNT,0,0)>0;
    EnableWindow(enableButton,any); EnableWindow(pauseButton,any); EnableWindow(unlockButton,any);
    EnableWindow(allEnableButton,n>0); EnableWindow(allPauseButton,n>0);
    UpdateBadge(); UpdateTray();
}
static void SelectManaged(HWND h) {
    SendMessageW(managedBox,LB_SETSEL,FALSE,-1);
    LRESULT count=SendMessageW(managedBox,LB_GETCOUNT,0,0);
    for(int k=0;k<count;++k) {
        LRESULT slot=SendMessageW(managedBox,LB_GETITEMDATA,k,0);
        if(slot>=0&&slot<MaxTargets && targets[slot].hwnd==h) {
            SendMessageW(managedBox,LB_SETSEL,TRUE,k); SendMessageW(managedBox,LB_SETCARETINDEX,k,FALSE); break;
        }
    }
    detailWindow=h; detailSlot=FindTarget(h); UpdateManaged(); UpdateDetail(true);
}
static void DetachTarget(Target& t) {
    HWND old=t.hwnd;
    if(t.objectHook) UnhookWinEvent(t.objectHook); if(t.systemHook) UnhookWinEvent(t.systemHook);
    if(t.process) CloseHandle(t.process);
    // Preserve only configuration; discard all state belonging to the old HWND.
    Target kept{}; kept.saved=t.saved; kept.width=t.width; kept.height=t.height;
    kept.enabled=t.enabled; kept.screenSized=t.screenSized;
    StringCchCopyW(kept.executable,1024,t.executable); StringCchCopyW(kept.windowClass,256,t.windowClass); StringCchCopyW(kept.title,256,t.title);
    t=kept;
    if(detailWindow==old) detailWindow=nullptr;
}
static void ReleaseTarget(Target& t) {
    int i=static_cast<int>(&t-targets); DetachTarget(t); t=Target{};
    if(detailSlot==i) { detailSlot=-1; detailWindow=nullptr; }
}
static void UnlockAll(const wchar_t* message) {
    for(auto& t:targets) ReleaseTarget(t);
    SaveSettings();
    KillTimer(mainWindow,TIMER_SETTLE); UpdateManaged(); UpdateDetail(true); Status(message);
}
static void QueueCheck() {
    if(!queued) { queued=true; PostMessageW(mainWindow,WM_CHECK,0,0); }
}
static void CALLBACK WindowEvent(HWINEVENTHOOK,DWORD event,HWND hwnd,LONG object,LONG child,DWORD,DWORD) {
    int i=FindTarget(hwnd); if(i<0) return;
    if(event>=EVENT_OBJECT_CREATE && (object!=OBJID_WINDOW||child!=CHILDID_SELF)) return;
    auto& t=targets[i]; ULONGLONG now=GetTickCount64();
    if(event==EVENT_OBJECT_DESTROY) t.destroyed=true;
    if(event==EVENT_SYSTEM_MOVESIZESTART) {
        t.moving=true; t.movingTitle=false;
        t.remaxPending=false; t.remaxAttempts=0;
        GetWindowRect(hwnd,&t.moveStart);
        POINT p; DWORD_PTR hit=0;
        if(GetCursorPos(&p) && SendMessageTimeoutW(hwnd,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y),SMTO_ABORTIFHUNG|SMTO_BLOCK,50,&hit))
            t.movingTitle=hit==HTCAPTION;
    }
    if(event==EVENT_SYSTEM_MOVESIZEEND) {
        t.moving=false; t.settleUntil=now+400;
        POINT p; auto m=MonitorInfo(hwnd); RECT end={};
        // Out-of-context events can arrive after the cursor has left the title.
        // A translated rectangle of unchanged size also identifies a move.
        bool translated=GetWindowRect(hwnd,&end) &&
            end.right-end.left==t.moveStart.right-t.moveStart.left &&
            end.bottom-end.top==t.moveStart.bottom-t.moveStart.top &&
            (end.left!=t.moveStart.left || end.top!=t.moveStart.top);
        if(t.enabled && !t.captureMax && !IsZoomed(hwnd) && (t.screenSized||ScreenDimensions(t)) && (t.movingTitle||translated) &&
            GetCursorPos(&p) && p.x>=m.rcWork.left && p.x<m.rcWork.right &&
            p.y>=m.rcMonitor.top && p.y<=m.rcWork.top+2) {
            ShowWindowAsync(hwnd,SW_MAXIMIZE);
            Log(L"TOP_EDGE_MAXIMIZE");
        }
        t.movingTitle=false;
    }
    if(event==EVENT_SYSTEM_MINIMIZEEND) t.settleUntil=now+200;
    QueueCheck();
}
static void BindTarget(Target& t,HWND hwnd) {
    t.hwnd=hwnd; GetWindowThreadProcessId(hwnd,&t.pid);
    t.process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,t.pid);
    t.objectHook=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_LOCATIONCHANGE,nullptr,WindowEvent,t.pid,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    t.systemHook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_MINIMIZEEND,nullptr,WindowEvent,t.pid,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    t.settleUntil=GetTickCount64()+400;
    if(DetailIndex()==static_cast<int>(&t-targets)) detailWindow=hwnd;
    Log(L"BIND_SAVED_TARGET"); QueueCheck();
}
static void ReconnectTargets() {
    bool waiting=false; for(const auto& t:targets) if(t.saved&&!t.hwnd) { waiting=true; break; }
    if(!waiting) return;
    struct Discovery { HWND match[MaxTargets]{}; int count[MaxTargets]{}; } discovery;
    EnumWindows([](HWND h,LPARAM data)->BOOL {
        if(!IsWindowVisible(h)||FindTarget(h)>=0) return TRUE;
        DWORD pid=0; GetWindowThreadProcessId(h,&pid); if(pid==GetCurrentProcessId()) return TRUE;
        if(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOOLWINDOW) return TRUE;
        Target identity{}; if(!ReadIdentity(h,identity)) return TRUE;
        auto& d=*reinterpret_cast<Discovery*>(data);
        for(int i=0;i<MaxTargets;++i) if(targets[i].saved&&!targets[i].hwnd&&SameIdentity(targets[i],identity)) { d.match[i]=h; ++d.count[i]; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&discovery));
    for(int i=0;i<MaxTargets;++i) if(discovery.count[i]==1) {
        int records=0; for(const auto& t:targets) if(t.saved&&SameIdentity(t,targets[i])) ++records;
        // Neither duplicate saved identities nor multiple live matches are safe.
        if(records==1&&FindTarget(discovery.match[i])<0) BindTarget(targets[i],discovery.match[i]);
    }
}
static void Observe(Target& t,const RECT& r,bool minimized,bool maximized) {
    if(logFile==INVALID_HANDLE_VALUE) return;
    if(!t.haveLog||!EqualRect(&r,&t.logRect)||minimized!=t.logMin||maximized!=t.logMax) {
        wchar_t text[512];
        StringCchPrintfW(text,512,L"OBSERVE hwnd=%p x=%ld y=%ld width=%ld height=%ld min=%d max=%d enabled=%d want=%dx%d",
            t.hwnd,r.left,r.top,r.right-r.left,r.bottom-r.top,minimized,maximized,t.enabled,t.width,t.height);
        Log(text); t.logRect=r; t.logMin=minimized; t.logMax=maximized; t.haveLog=true;
    }
}
static void CheckTargets() {
    queued=false; ULONGLONG now=GetTickCount64(); bool soon=false;
    for(auto& t:targets) {
        if(!t.hwnd) continue;
        if(!TargetValid(t)) { DetachTarget(t); Status(L"目标已关闭，尺寸和启用状态已保留；相同窗口打开后自动恢复。"); continue; }
        wchar_t title[256]; NormalizedTitle(t.hwnd,title);
        if(title[0]&&lstrcmpW(t.title,title)!=0) { StringCchCopyW(t.title,256,title); settingsDirty=true; }
        RECT r; if(!GetWindowRect(t.hwnd,&r)) continue;
        bool min=IsIconic(t.hwnd)!=FALSE,max=IsZoomed(t.hwnd)!=FALSE;
        Observe(t,r,min,max);
        if(!t.haveGeometry || !EqualRect(&r,&t.geometry) || min!=t.geometryMin || max!=t.geometryMax) {
            t.geometry=r; t.geometryMin=min; t.geometryMax=max; t.geometrySince=now; t.haveGeometry=true;
        }
        bool stable=now-t.geometrySince>=300;
        if(t.remaxPending) {
            if(min||!t.enabled) { t.remaxPending=false; }
            else if(!max && stable) {
                t.remaxPending=false; ShowWindowAsync(t.hwnd,SW_MAXIMIZE); t.settleUntil=now+400;
                Log(L"REAPPLY_MAXIMIZE"); soon=true; continue;
            } else if(now<t.remaxUntil) { soon=true; continue; }
            else { t.remaxPending=false; t.enabled=false; settingsDirty=true; Status(L"目标未响应最大化位置修复，已暂停；可重新启用或检查目标权限。"); continue; }
        }
        if(t.captureMax) {
            if(max && stable && FillsWorkArea(t.hwnd,r)) {
                t.width=r.right-r.left; t.height=r.bottom-r.top; t.captureMax=false; t.screenSized=true;
                SaveSettings(); if(detailWindow==t.hwnd) FillInputs(); Status(L"已记录稳定的最大化尺寸，并核对窗口铺满工作区。");
            } else if(now<t.captureUntil) { soon=true; continue; }
            else { t.captureMax=false; Status(L"目标未响应最大化，请检查目标程序是否支持最大化。"); }
        }
        if(!t.enabled||min||!IsWindowVisible(t.hwnd)||t.moving) { t.mismatchSince=0; continue; }
        if(now<t.settleUntil || !stable || r.left<=-30000 || r.top<=-30000) { soon=true; continue; }
        // Matching maximized dimensions are valid. Never restore a maximized
        // window solely because IsZoomed is true (the v1.1 regression).
        bool wrong=max?!MaxSizeMatches(t,r):r.right-r.left!=t.width||r.bottom-r.top!=t.height;
        bool repairMax=max && MaxSizeMatches(t,r) && !FillsWorkArea(t.hwnd,r);
        if(repairMax) wrong=true;
        if(!wrong) { t.mismatchSince=0; if(max) t.remaxAttempts=0; continue; }
        if(!t.mismatchSince) t.mismatchSince=now;
        DWORD interval=now-t.mismatchSince>3000?500:60;
        if(t.lastAttempt && now-t.lastAttempt<interval) { soon=true; continue; }
        t.lastAttempt=now; BOOL success;
        if(repairMax) {
            // Clear a malformed maximized placement through Windows itself.
            // Wait for the restore to settle before issuing maximize again.
            if(t.remaxAttempts>=2) {
                t.enabled=false; settingsDirty=true; Status(L"目标最大化后仍未覆盖工作区，已暂停该目标。可检查目标的尺寸限制后重新启用。"); continue;
            }
            ++t.remaxAttempts;
            success=ShowWindowAsync(t.hwnd,SW_RESTORE);
            t.remaxPending=success!=FALSE; t.remaxUntil=now+2500;
            Log(L"REPAIR_MAXIMIZED_PLACEMENT");
        } else if(max) {
            success=ShowWindowAsync(t.hwnd,SW_RESTORE); t.settleUntil=now+200;
            wchar_t text[128]; StringCchPrintfW(text,128,L"RESTORE_FROM_MAXIMIZED hwnd=%p",t.hwnd); Log(text);
        } else {
            success=SetWindowPos(t.hwnd,nullptr,0,0,t.width,t.height,
                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_ASYNCWINDOWPOS);
            wchar_t text[256]; StringCchPrintfW(text,256,L"CORRECT hwnd=%p width=%d height=%d accepted=%d",t.hwnd,t.width,t.height,success); Log(text);
        }
        if(!success) { t.enabled=false; settingsDirty=true; Status(L"一个目标无法调整，已暂停该目标；请检查是否需要与目标相同的权限。"); }
        else if(now-t.mismatchSince>3000) Status(L"目标限制尺寸或持续改写大小，正在降低频率重试。可暂停该目标。");
        soon=true;
    }
    UpdateManaged(); UpdateDetail();
    if(soon) SetTimer(mainWindow,TIMER_SETTLE,80,nullptr);
}
static bool LockWindow(HWND hwnd,DWORD expectedPid=0) {
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    if(!hwnd||!IsWindow(hwnd)||!pid||(expectedPid&&pid!=expectedPid)||pid==GetCurrentProcessId()||!IsWindowVisible(hwnd)) {
        Status(L"这个窗口已关闭或无法选取，请刷新后重试。"); return false;
    }
    int i=FindTarget(hwnd);
    if(i>=0) { SelectManaged(hwnd); Status(L"这个窗口已在管理列表中。可修改尺寸或启用、暂停。"); return true; }
    if(IsIconic(hwnd)) { Status(L"请先还原最小化窗口，再读取并固定尺寸。"); return false; }
    RECT r; if(!GetWindowRect(hwnd,&r)||r.right<=r.left||r.bottom<=r.top) return false;
    Target identity{};
    if(!ReadIdentity(hwnd,identity)) { Status(L"无法读取程序路径或窗口标题，请检查目标权限后重试。"); return false; }
    int waiting=-1,matches=0;
    for(i=0;i<MaxTargets;++i) if(targets[i].saved&&!targets[i].hwnd&&SameIdentity(targets[i],identity)) { waiting=i; ++matches; }
    if(matches>1) { Status(L"有多条相同窗口记录，请移除重复记录后重新选取。"); return false; }
    if(matches==1) { BindTarget(targets[waiting],hwnd); UpdateManaged(); SelectManaged(hwnd); Status(L"已重新连接保存的记录，保留原尺寸和启用状态。"); return true; }
    for(i=0;i<MaxTargets;++i) if(!targets[i].saved) break;
    if(i==MaxTargets) { Status(L"最多同时管理 32 个窗口。请先移除不需要的目标。"); return false; }
    auto& t=targets[i]; t=identity; t.saved=true; t.hwnd=hwnd; t.pid=pid; t.enabled=true;
    t.width=r.right-r.left; t.height=r.bottom-r.top;
    t.screenSized=IsZoomed(hwnd)!=FALSE || ScreenDimensions(t);
    if(IsZoomed(hwnd)) { t.captureMax=true; t.captureUntil=GetTickCount64()+2500; }
    BindTarget(t,hwnd); SaveSettings();
    wchar_t text[256]; StringCchPrintfW(text,256,L"LOCK hwnd=%p pid=%lu width=%d height=%d",hwnd,pid,t.width,t.height); Log(text);
    UpdateManaged(); SelectManaged(hwnd); QueueCheck();
    Status(IsZoomed(hwnd)?L"已加入固定，并保留最大化状态。最大化尺寸不会再被强制还原。":L"已加入固定。可继续添加其他窗口，或在右侧修改尺寸。");
    return true;
}
static BOOL CALLBACK Enumerate(HWND hwnd,LPARAM) {
    if(candidateCount>=512||!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid); if(pid==GetCurrentProcessId()) return TRUE;
    LONG_PTR ex=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    if((ex&WS_EX_TOOLWINDOW)&&!(ex&WS_EX_APPWINDOW)) return TRUE;
    wchar_t title[256]={}; GetWindowTextW(hwnd,title,256); if(!title[0]) return TRUE;
    candidates[candidateCount]={hwnd,pid};
    LRESULT index=SendMessageW(listBox,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    if(index!=LB_ERR&&index!=LB_ERRSPACE) SendMessageW(listBox,LB_SETITEMDATA,index,candidateCount++); return TRUE;
}
static void RefreshList() {
    SendMessageW(listBox,WM_SETREDRAW,FALSE,0); SendMessageW(listBox,LB_RESETCONTENT,0,0); candidateCount=0;
    EnumWindows(Enumerate,0); SendMessageW(listBox,WM_SETREDRAW,TRUE,0); InvalidateRect(listBox,nullptr,TRUE);
    EnableWindow(lockButton,FALSE);
}
static void PreviewSelected(HWND box) {
    LRESULT caret=SendMessageW(box,LB_GETCARETINDEX,0,0);
    LRESULT data=caret==LB_ERR?LB_ERR:SendMessageW(box,LB_GETITEMDATA,caret,0);
    if(box==listBox) { if(data>=0&&data<candidateCount) { detailWindow=candidates[data].hwnd; detailSlot=FindTarget(detailWindow); } }
    else if(data>=0&&data<MaxTargets) { detailSlot=static_cast<int>(data); detailWindow=targets[data].hwnd; }
    UpdateDetail(true); UpdateManaged();
}
static void LockSelected() {
    int indices[512]; LRESULT count=SendMessageW(listBox,LB_GETSELITEMS,512,reinterpret_cast<LPARAM>(indices));
    if(count<=0) { Status(L"请选择一个或多个窗口。Ctrl / Shift 可多选。"); return; }
    for(int k=0;k<count;++k) {
        LRESULT data=SendMessageW(listBox,LB_GETITEMDATA,indices[k],0);
        if(data>=0&&data<candidateCount) LockWindow(candidates[data].hwnd,candidates[data].pid);
    }
}
static void ManageSelected(int action) {
    int indices[MaxTargets]; LRESULT count=SendMessageW(managedBox,LB_GETSELITEMS,MaxTargets,reinterpret_cast<LPARAM>(indices));
    for(int k=0;k<count;++k) {
        LRESULT i=SendMessageW(managedBox,LB_GETITEMDATA,indices[k],0); if(i<0||i>=MaxTargets) continue;
        auto& t=targets[i];
        if(action==ID_UNLOCK) ReleaseTarget(t);
        else { t.enabled=action==ID_ENABLE; t.captureMax=false; t.remaxPending=false; t.remaxAttempts=0; t.mismatchSince=0; t.lastAttempt=0; }
    }
    SaveSettings(); UpdateManaged(); UpdateDetail(true); QueueCheck();
    Status(action==ID_UNLOCK?L"已移除所选目标。其余窗口继续固定。":action==ID_ENABLE?L"已启用所选目标，恢复各自的固定尺寸。":L"已暂停所选目标，可自由调整；其他窗口继续固定。");
}
static bool ReadDimension(HWND edit,int& value) {
    wchar_t text[32]; GetWindowTextW(edit,text,32); if(!text[0]) return false;
    value=0; for(const wchar_t* p=text;*p;++p) { if(*p<L'0'||*p>L'9') return false; value=value*10+(*p-L'0'); if(value>32767) return false; }
    return value>=1;
}
static void ApplySize(bool useMax) {
    int i=DetailIndex(); HWND h=i>=0?targets[i].hwnd:detailWindow; int w=0,height=0;
    if(!useMax && (!ReadDimension(widthEdit,w)||!ReadDimension(heightEdit,height))) {
        Status(L"宽、高请输入 1–32767 的整数像素值。目标程序可能另有最小尺寸限制。"); return;
    }
    if(useMax&&(!h||!IsWindow(h))) { Status(L"请先打开目标窗口，再读取最大化尺寸。"); return; }
    if(i<0) { if(!h||!IsWindow(h)||!LockWindow(h)) return; i=FindTarget(h); }
    if(i<0) return; auto& t=targets[i];
    t.enabled=true; t.remaxPending=false; t.remaxAttempts=0; t.mismatchSince=0; t.lastAttempt=0;
    if(useMax) {
        t.captureMax=true; t.captureUntil=GetTickCount64()+2500;
        if(!ShowWindowAsync(h,SW_MAXIMIZE)) { t.captureMax=false; Status(L"无法最大化目标窗口，请检查目标权限。"); return; }
        Status(L"正在读取目标实际最大化尺寸…");
    } else {
        t.captureMax=false; t.width=w; t.height=height;
        RECT current={};
        t.screenSized=h&&(ScreenDimensions(t) ||
            (IsZoomed(h)&&GetWindowRect(h,&current)&&w==current.right-current.left&&height==current.bottom-current.top));
        Status(h?L"新尺寸已应用并启用固定，仅修改当前查看的窗口。":L"新尺寸已保存并启用，目标窗口重新打开后自动应用。");
    }
    SaveSettings(); UpdateManaged(); UpdateDetail(true); QueueCheck();
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
    Rounded(dc,{Px(24),Px(208),Px(440),Px(390)},Card,Border,12);
    Rounded(dc,{Px(24),Px(452),Px(440),Px(572)},Card,Border,12);
    Rounded(dc,{Px(460),Px(164),Px(856),Px(612)},Card,Border,14);
    COLORREF fill=badgeState==BadgeState::Active?RGB(220,252,231):badgeState==BadgeState::Paused?RGB(254,243,199):badgeState==BadgeState::Picking?RGB(219,234,254):RGB(229,231,235);
    Rounded(dc,BadgeRect(),fill,fill,20);
}
static void DrawButton(const DRAWITEMSTRUCT& item) {
    bool enabled=(item.itemState&ODS_DISABLED)==0, pressed=(item.itemState&ODS_SELECTED)!=0;
    bool hover=item.hwndItem==hoveredButton;
    bool primary=item.CtlID==ID_PICK||item.CtlID==ID_APPLY;
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
    wchar_t title[512]={}; SendMessageW(item.hwndItem,LB_GETTEXT,item.itemID,reinterpret_cast<LPARAM>(title));
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
    auto move=[](HWND h,int x,int y,int w,int height) { MoveWindow(h,Px(x),Px(y),Px(w),Px(height),TRUE); };
    move(brandLabel,58,17,560,32); move(hintLabel,24,56,832,20);
    RECT badge=BadgeRect(); MoveWindow(badgeLabel,badge.left,badge.top+Px(4),badge.right-badge.left,Px(20),TRUE);
    move(pickerButton,24,96,832,48);
    move(listCaption,24,176,270,22); move(refreshButton,326,168,114,30);
    move(listBox,28,212,408,174); move(lockButton,24,396,416,34);
    move(managedCaption,24,434,416,18); move(managedBox,28,456,408,112);
    move(enableButton,24,582,132,34); move(pauseButton,166,582,132,34); move(unlockButton,308,582,132,34);
    move(allEnableButton,24,626,132,32); move(allPauseButton,166,626,132,32); move(hideButton,308,626,132,32);
    move(targetCaption,480,180,356,18); move(targetLabel,480,204,356,28);
    move(currentCaption,480,250,174,18); move(wantedCaption,662,250,174,18);
    move(currentSizeLabel,480,274,174,28); move(sizeLabel,662,274,174,28);
    move(visibleLabel,480,316,356,22); move(screenLabel,480,354,356,22); move(workLabel,480,384,356,22);
    move(widthCaption,480,430,118,18); move(heightCaption,606,430,118,18);
    move(widthEdit,480,453,118,34); move(heightEdit,606,453,118,34); move(applyButton,732,453,104,34);
    move(maxSizeButton,480,503,356,34); move(modeLabel,480,551,356,20); move(metricNote,480,578,356,26);
    move(statusLabel,460,626,396,38); move(shortcutLabel,24,678,716,18); move(exitButton,756,670,100,30);
    SendMessageW(listBox,LB_SETITEMHEIGHT,0,Px(29)); SendMessageW(managedBox,LB_SETITEMHEIGHT,0,Px(28)); InvalidateRect(mainWindow,nullptr,FALSE);
}
static void RecreateFonts() {
    HFONT old=font, oldTitle=titleFont, oldTarget=targetFont, oldMetric=metricFont, oldSmall=smallFont;
    font=CreateFontW(-Px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    titleFont=CreateFontW(-Px(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    targetFont=CreateFontW(-Px(17),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    metricFont=CreateFontW(-Px(20),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    smallFont=CreateFontW(-Px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HWND controls[]={listBox,managedBox,statusLabel,lockButton,unlockButton,pickerButton,refreshButton,hideButton,exitButton,listCaption,managedCaption,enableButton,pauseButton,allEnableButton,allPauseButton,widthEdit,heightEdit,applyButton,maxSizeButton,visibleLabel,screenLabel,workLabel};
    for(HWND h:controls) if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    HWND smallControls[]={hintLabel,badgeLabel,targetCaption,wantedCaption,currentCaption,shortcutLabel,widthCaption,heightCaption,modeLabel,metricNote};
    for(HWND h:smallControls) if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
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
    AppendMenuW(menu,MF_STRING|(CountTargets()?0:MF_GRAYED),ID_TRAY_UNLOCK,L"解除全部固定");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,ID_TRAY_EXIT,L"退出（保留记录）");
    POINT p; GetCursorPos(&p); SetForegroundWindow(mainWindow);
    UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,mainWindow,nullptr);
    DestroyMenu(menu); PostMessageW(mainWindow,WM_NULL,0,0);
    if(command==ID_TRAY_OPEN) ShowMain();
    else if(command==ID_TRAY_UNLOCK) UnlockAll(L"已解除全部固定，可正常调整窗口大小。");
    else if(command==ID_TRAY_EXIT) DestroyWindow(mainWindow);
}

static LRESULT CALLBACK MainProcedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==taskbarCreated) { trayAdded=false; AddTray(); return 0; }
    switch(message) {
    case WM_CREATE: {
        mainWindow=hwnd; if(dpiForWindow) dpi=static_cast<int>(dpiForWindow(hwnd)); RecreateFonts();
        auto label=[](const wchar_t* text,DWORD style=SS_LEFT) { return Control(L"STATIC",text,style,0); };
        auto button=[](const wchar_t* text,int id) { return Control(L"BUTTON",text,WS_TABSTOP|BS_OWNERDRAW,id); };
        brandLabel=label(L"窗口尺寸固定"); hintLabel=label(L"选取即固定；记录自动保存，窗口关闭后保留，重新打开时自动恢复。");
        badgeLabel=label(L"启用 0 / 0",SS_CENTER); targetCaption=label(L"窗口详情 · 所有尺寸均为物理像素");
        targetLabel=label(L"选择窗口，查看尺寸或修改",SS_LEFT|SS_ENDELLIPSIS);
        wantedCaption=label(L"设定外部尺寸 · px"); currentCaption=label(L"当前外部尺寸 · px");
        sizeLabel=label(L"尚未固定"); currentSizeLabel=label(L"— × —");
        visibleLabel=label(L"可见外框：—"); screenLabel=label(L"屏幕分辨率：—"); workLabel=label(L"最大化工作区：—");
        widthCaption=label(L"外部宽度 · px");
        widthEdit=Control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,ID_WIDTH);
        heightCaption=label(L"外部高度 · px");
        heightEdit=Control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,ID_HEIGHT);
        SendMessageW(widthEdit,EM_SETLIMITTEXT,5,0); SendMessageW(heightEdit,EM_SETLIMITTEXT,5,0);
        applyButton=button(L"应用尺寸",ID_APPLY); maxSizeButton=button(L"使用此窗口的最大化尺寸",ID_MAXSIZE);
        modeLabel=label(L"未选择"); metricNote=label(L"外部尺寸含隐形边框；工作区已扣除任务栏。");
        pickerButton=button(L"按住拖到目标窗口 · 松开即可加入固定",ID_PICK);
        DWORD listStyle=WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_EXTENDEDSEL;
        listCaption=label(L"可选窗口 · Ctrl / Shift 多选");
        listBox=Control(L"LISTBOX",L"可选窗口",listStyle,ID_LIST);
        managedCaption=label(L"固定管理 · 多选后可只启用其中一部分");
        managedBox=Control(L"LISTBOX",L"固定管理",listStyle,ID_MANAGED);
        lockButton=button(L"加入固定 · 所选窗口",ID_LOCK); refreshButton=button(L"刷新列表",ID_REFRESH);
        enableButton=button(L"启用所选",ID_ENABLE); pauseButton=button(L"暂停所选",ID_PAUSE); unlockButton=button(L"移除所选",ID_UNLOCK);
        allEnableButton=button(L"全部启用",ID_ALL_ENABLE); allPauseButton=button(L"全部暂停",ID_ALL_PAUSE);
        hideButton=button(L"收起到托盘",ID_HIDE); exitButton=button(L"退出",ID_EXIT);
        statusLabel=label(L"拖动选取或双击列表即可固定。最多管理 32 个窗口。");
        shortcutLabel=label(L"Ctrl + Alt + Z 加入前台窗口   ·   Ctrl + Alt + U 解除前台窗口");
        RecreateFonts();
        HWND buttons[]={pickerButton,lockButton,refreshButton,unlockButton,hideButton,exitButton,enableButton,pauseButton,allEnableButton,allPauseButton,applyButton,maxSizeButton};
        for(HWND h:buttons) SetWindowSubclass(h,ButtonProcedure,2,0);
        SetWindowSubclass(pickerButton,PickerProcedure,1,0); InitializeSettings(); LoadSettings(); RefreshList(); ReconnectTargets(); UpdateManaged(); UpdateDetail(true); Layout();
        captureHotkey=RegisterHotKey(hwnd,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'Z')!=FALSE;
        unlockHotkey=RegisterHotKey(hwnd,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'U')!=FALSE;
        if(!captureHotkey||!unlockHotkey) Status(L"部分快捷键被占用，请使用拖动选取或列表。");
        SetTimer(hwnd,TIMER_WATCH,1000,nullptr); AddTray(); return 0;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case ID_LOCK: LockSelected(); break;
        case ID_LIST:
            if(HIWORD(wp)==LBN_DBLCLK) LockSelected();
            else if(HIWORD(wp)==LBN_SELCHANGE) { EnableWindow(lockButton,SendMessageW(listBox,LB_GETSELCOUNT,0,0)>0); PreviewSelected(listBox); } break;
        case ID_MANAGED: if(HIWORD(wp)==LBN_SELCHANGE) PreviewSelected(managedBox); break;
        case ID_UNLOCK: case ID_ENABLE: case ID_PAUSE: ManageSelected(LOWORD(wp)); break;
        case ID_ALL_ENABLE: case ID_ALL_PAUSE:
            for(auto& t:targets) if(t.saved) { t.enabled=LOWORD(wp)==ID_ALL_ENABLE; t.captureMax=false; t.remaxPending=false; t.remaxAttempts=0; t.lastAttempt=0; t.mismatchSince=0; }
            SaveSettings(); UpdateManaged(); QueueCheck(); Status(LOWORD(wp)==ID_ALL_ENABLE?L"已启用全部目标，各自恢复记录尺寸。":L"已暂停全部目标，固定尺寸记录仍保留。"); break;
        case ID_APPLY: ApplySize(false); break;
        case ID_MAXSIZE: ApplySize(true); break;
        case ID_REFRESH: RefreshList(); Status(L"列表已刷新，单击可查看尺寸，双击加入固定。"); break;
        case ID_PICK: Status(L"按住按钮，拖到目标窗口后松开；新窗口会追加到管理列表。"); break;
        case ID_HIDE: if(trayAdded) ShowWindow(hwnd,SW_HIDE); else Status(L"托盘暂不可用，请使用最小化按钮。"); break;
        case ID_EXIT: DestroyWindow(hwnd); break;
        } return 0;
    case WM_CHECK: CheckTargets(); return 0;
    case WM_HOTKEY:
        if(wp==1) LockWindow(GetForegroundWindow());
        else if(wp==2) {
            int i=FindTarget(GetForegroundWindow());
            if(i>=0) { ReleaseTarget(targets[i]); SaveSettings(); UpdateManaged(); UpdateDetail(true); Status(L"已解除前台窗口并删除记录，其余窗口继续固定。"); }
        } return 0;
    case WM_SETFOCUS: if(listBox) SetFocus(listBox); return 0;
    case WM_TIMER:
        if(wp==TIMER_SETTLE) KillTimer(hwnd,TIMER_SETTLE);
        if(wp==TIMER_WATCH) { ReconnectTargets(); if(settingsDirty&&GetTickCount64()-saveAttempt>=1000) SaveSettings(); }
        if(wp==TIMER_WATCH||wp==TIMER_SETTLE) CheckTargets(); return 0;
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
        if(item->CtlID==ID_LIST||item->CtlID==ID_MANAGED) { item->itemHeight=Px(29); return TRUE; } break;
    }
    case WM_DRAWITEM: {
        auto item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(item->CtlID==ID_LIST||item->CtlID==ID_MANAGED) DrawListRow(*item); else DrawButton(*item);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc=BeginPaint(hwnd,&paint); PaintBackground(dc); EndPaint(hwnd,&paint); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wp); HWND label=reinterpret_cast<HWND>(lp);
        SetBkMode(dc,TRANSPARENT);
        bool inCard=label==targetLabel||label==sizeLabel||label==currentSizeLabel||label==targetCaption||label==wantedCaption||label==currentCaption||label==visibleLabel||label==screenLabel||label==workLabel||label==widthCaption||label==heightCaption||label==modeLabel||label==metricNote;
        COLORREF ink=label==brandLabel||label==targetLabel||label==sizeLabel?Ink:label==currentSizeLabel?Accent:Muted;
        if(label==badgeLabel) ink=badgeState==BadgeState::Active?RGB(21,128,61):badgeState==BadgeState::Paused?RGB(146,64,14):badgeState==BadgeState::Picking?Accent:Muted;
        SetTextColor(dc,ink);
        return reinterpret_cast<LRESULT>(label==badgeLabel?GetStockObject(NULL_BRUSH):inCard?cardBrush:backgroundBrush);
    }
    case WM_CTLCOLORLISTBOX: SetBkColor(reinterpret_cast<HDC>(wp),Card); return reinterpret_cast<LRESULT>(cardBrush);
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        if(settingsDirty) SaveSettings();
        for(auto& t:targets) DetachTarget(t); KillTimer(hwnd,TIMER_SETTLE); KillTimer(hwnd,TIMER_WATCH);
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
    RECT r={0,0,Px(880),Px(716)};
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
