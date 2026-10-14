#include <windows.h>
#include <commdlg.h>
#include "qemupwr_core.h"

#define IDC_INSTALLED   201
#define IDC_RUNNING     202
#define IDC_STARTUP     203
#define IDC_DEVICE      204
#define IDC_COMPAT      205
#define IDC_INSTALL     301
#define IDC_REMOVE      302
#define IDC_INFO        303
#define IDC_INFO_OS     401
#define IDC_INFO_KERNEL 402
#define IDC_INFO_DEVICE 403
#define IDC_INFO_EVENTS 404
#define IDC_INFO_COMPAT 405
#define IDC_INFO_BRANCH 406
#define IDC_INFO_IAT    407
#define IDC_INFO_NOTE   408
#define IDC_INFO_SAVE   409
#define IDC_INFO_REFRESH 410

#define DLG_BYTES 4096

static DWORD g_MainData[DLG_BYTES / sizeof(DWORD)];
static DWORD g_InfoData[DLG_BYTES / sizeof(DWORD)];
static BYTE *g_Template;
static DWORD g_Offset;

static void AddWord(WORD Value)
{
    g_Template[g_Offset++] = (BYTE)Value;
    g_Template[g_Offset++] = (BYTE)(Value >> 8);
}

static void AddDword(DWORD Value)
{
    AddWord((WORD)(Value & 0xFFFFUL));
    AddWord((WORD)(Value >> 16));
}

static void AddText(LPCSTR Text)
{
    while (*Text) AddWord((WORD)(BYTE)*Text++);
    AddWord(0);
}

static void AlignDword(void)
{
    while (g_Offset & 3) g_Template[g_Offset++] = 0;
}

static void AddControl(DWORD Style, WORD X, WORD Y, WORD W, WORD H,
                       WORD Id, WORD Class, LPCSTR Caption)
{
    AlignDword();
    AddDword(Style | WS_CHILD | WS_VISIBLE);
    AddDword(0);
    AddWord(X); AddWord(Y); AddWord(W); AddWord(H);
    AddWord(Id);
    AddWord(0xFFFF); AddWord(Class);
    AddText(Caption);
    AddWord(0);
}

static void BeginTemplate(DWORD *Storage, WORD Controls,
                          WORD Width, WORD Height, LPCSTR Caption)
{
    g_Template = (BYTE *)Storage;
    g_Offset = 0;
    AddDword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME);
    AddDword(0);
    AddWord(Controls);
    AddWord(0); AddWord(0); AddWord(Width); AddWord(Height);
    AddWord(0); AddWord(0);
    AddText(Caption);
}

static DLGTEMPLATE *CreateMainTemplate(void)
{
    BeginTemplate(g_MainData, 15, 268, 155, "QEMU pvpanic Driver");
    AddControl(BS_GROUPBOX, 8, 8, 252, 110, 101, 0x80, "Status");
    AddControl(SS_LEFT, 18, 24, 68, 12, 110, 0x82, "Driver:");
    AddControl(SS_LEFT, 89, 24, 158, 12, IDC_INSTALLED, 0x82, "");
    AddControl(SS_LEFT, 18, 42, 68, 12, 111, 0x82, "Service:");
    AddControl(SS_LEFT, 89, 42, 158, 12, IDC_RUNNING, 0x82, "");
    AddControl(SS_LEFT, 18, 60, 68, 12, 112, 0x82, "Startup:");
    AddControl(SS_LEFT, 89, 60, 158, 12, IDC_STARTUP, 0x82, "");
    AddControl(SS_LEFT, 18, 78, 68, 12, 113, 0x82, "Device:");
    AddControl(SS_LEFT, 89, 78, 158, 12, IDC_DEVICE, 0x82, "");
    AddControl(SS_LEFT, 18, 96, 68, 12, 114, 0x82, "Compatibility:");
    AddControl(SS_LEFT, 89, 96, 158, 12, IDC_COMPAT, 0x82, "");

    AddControl(BS_PUSHBUTTON | WS_TABSTOP, 8, 129, 58, 17,
               IDC_INSTALL, 0x80, "Install");
    AddControl(BS_PUSHBUTTON | WS_TABSTOP, 71, 129, 58, 17,
               IDC_REMOVE, 0x80, "Uninstall");
    AddControl(BS_PUSHBUTTON | WS_TABSTOP, 134, 129, 58, 17,
               IDC_INFO, 0x80, "Info...");
    AddControl(BS_DEFPUSHBUTTON | WS_TABSTOP, 197, 129, 63, 17,
               IDCANCEL, 0x80, "Close");
    return (DLGTEMPLATE *)g_MainData;
}

static DLGTEMPLATE *CreateInfoTemplate(void)
{
    BeginTemplate(g_InfoData, 21, 280, 216, "QEMU pvpanic Driver - Information");
    AddControl(BS_GROUPBOX, 8, 8, 264, 47, 500, 0x80, "System");
    AddControl(SS_LEFT, 18, 23, 65, 12, 501, 0x82, "Windows:");
    AddControl(SS_LEFT, 83, 23, 180, 12, IDC_INFO_OS, 0x82, "");
    AddControl(SS_LEFT, 18, 39, 65, 12, 502, 0x82, "Kernel:");
    AddControl(SS_LEFT, 83, 39, 180, 12, IDC_INFO_KERNEL, 0x82, "");

    AddControl(BS_GROUPBOX, 8, 60, 264, 51, 503, 0x80, "QEMU pvpanic ISA");
    AddControl(SS_LEFT, 18, 76, 65, 12, 504, 0x82, "Device:");
    AddControl(SS_LEFT, 83, 76, 180, 12, IDC_INFO_DEVICE, 0x82, "");
    AddControl(SS_LEFT, 18, 92, 65, 12, 505, 0x82, "Events:");
    AddControl(SS_LEFT, 83, 92, 180, 12, IDC_INFO_EVENTS, 0x82, "");

    AddControl(BS_GROUPBOX, 8, 116, 264, 59, 506, 0x80, "Kernel compatibility");
    AddControl(SS_LEFT, 18, 130, 65, 12, 507, 0x82, "Status:");
    AddControl(SS_LEFT, 83, 130, 180, 12, IDC_INFO_COMPAT, 0x82, "");
    AddControl(SS_LEFT, 18, 145, 65, 12, 508, 0x82, "Kernel path:");
    AddControl(SS_LEFT, 83, 145, 180, 12, IDC_INFO_BRANCH, 0x82, "");
    AddControl(SS_LEFT, 18, 160, 65, 12, 509, 0x82, "HAL import:");
    AddControl(SS_LEFT, 83, 160, 180, 12, IDC_INFO_IAT, 0x82, "");

    AddControl(SS_LEFT, 9, 179, 262, 12, IDC_INFO_NOTE, 0x82, "");
    AddControl(BS_PUSHBUTTON | WS_TABSTOP, 8, 193, 90, 17,
               IDC_INFO_SAVE, 0x80, "Save Report...");
    AddControl(BS_PUSHBUTTON | WS_TABSTOP, 104, 193, 76, 17,
               IDC_INFO_REFRESH, 0x80, "Refresh");
    AddControl(BS_DEFPUSHBUTTON | WS_TABSTOP, 202, 193, 70, 17,
               IDCANCEL, 0x80, "Close");
    return (DLGTEMPLATE *)g_InfoData;
}

static void CenterDialog(HWND Dlg)
{
    RECT Rect;
    int Width, Height;
    GetWindowRect(Dlg, &Rect);
    Width = Rect.right - Rect.left;
    Height = Rect.bottom - Rect.top;
    SetWindowPos(Dlg, NULL,
                 (GetSystemMetrics(SM_CXSCREEN) - Width) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - Height) / 2,
                 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

static void PutHex(HWND Dlg, int Control, DWORD Value)
{
    char Text[32];
    wsprintfA(Text, "0x%08lX", Value);
    SetDlgItemTextA(Dlg, Control, Text);
}

static void RefreshMain(HWND Dlg)
{
    QEMU_STATE State;
    QEMU_PROBE Probe;
    BOOL Available = QemuGetState(&State);
    BOOL Probed = QemuGetProbe(&Probe);
    BOOL CanInstall;
    if (!Available) {
        SetDlgItemTextA(Dlg, IDC_INSTALLED, "Unavailable");
        SetDlgItemTextA(Dlg, IDC_RUNNING, "Unknown");
        SetDlgItemTextA(Dlg, IDC_STARTUP, "Unknown");
        SetDlgItemTextA(Dlg, IDC_DEVICE, "Unknown");
        SetDlgItemTextA(Dlg, IDC_COMPAT, "Unknown");
        EnableWindow(GetDlgItem(Dlg, IDC_INSTALL), FALSE);
        EnableWindow(GetDlgItem(Dlg, IDC_REMOVE), FALSE);
        return;
    }
    SetDlgItemTextA(Dlg, IDC_INSTALLED,
                    State.Installed ? "Installed" : "Not installed");
    SetDlgItemTextA(Dlg, IDC_RUNNING,
                    !State.Installed ? "-" : !State.Running ? "Stopped" :
                    !State.AgentRunning ? "Agent stopped" : "Running");
    SetDlgItemTextA(Dlg, IDC_STARTUP,
                    !State.Installed ? "-" : State.Automatic ? "Automatic" : "Manual");

    if (Probed) {
        SetDlgItemTextA(Dlg, IDC_DEVICE,
                        !Probe.PvpanicPresent ? "Not detected" :
                        Probe.ShutdownSupported ? "pvpanic ISA (0x505)" :
                        "Shutdown unsupported");
        SetDlgItemTextA(Dlg, IDC_COMPAT,
                        !Probe.Compatible ? "Unsupported" :
                        State.Installed && !State.AgentRunning ?
                        "Winlogon agent stopped" :
                        !State.Installed ? "Kernel compatible" :
                        Probe.TestedKernel ? "Compatible" : "Experimental");
    } else {
        SetDlgItemTextA(Dlg, IDC_DEVICE, "Not checked");
        SetDlgItemTextA(Dlg, IDC_COMPAT, "Not checked");
    }
    CanInstall = (Probed && Probe.Compatible && Probe.TestedKernel &&
                  Probe.ShutdownSupported &&
                  (!State.Installed || !State.HookActive || !State.Automatic));
    EnableWindow(GetDlgItem(Dlg, IDC_INSTALL), CanInstall);
    EnableWindow(GetDlgItem(Dlg, IDC_REMOVE), State.Installed);
}

static void RefreshInfo(HWND Dlg)
{
    QEMU_PROBE Probe;
    QEMU_STATE State;
    OSVERSIONINFOA Version;
    char Text[256];
    BOOL Probed;
    ZeroMemory(&Version, sizeof(Version));
    Version.dwOSVersionInfoSize = sizeof(Version);
    if (GetVersionExA(&Version)) {
        wsprintfA(Text, "NT %lu.%lu (build %lu) %s",
                  Version.dwMajorVersion, Version.dwMinorVersion,
                  Version.dwBuildNumber & 0xFFFFUL, Version.szCSDVersion);
        SetDlgItemTextA(Dlg, IDC_INFO_OS, Text);
    } else {
        SetDlgItemTextA(Dlg, IDC_INFO_OS, "Unavailable");
    }
    Probed = QemuGetProbe(&Probe);
    ZeroMemory(&State, sizeof(State));
    QemuGetState(&State);
    if (!Probed) {
        SetDlgItemTextA(Dlg, IDC_INFO_KERNEL, "Unavailable");
        SetDlgItemTextA(Dlg, IDC_INFO_DEVICE, "Unavailable");
        SetDlgItemTextA(Dlg, IDC_INFO_EVENTS, "-");
        SetDlgItemTextA(Dlg, IDC_INFO_COMPAT, "Scan failed");
        SetDlgItemTextA(Dlg, IDC_INFO_BRANCH, "-");
        SetDlgItemTextA(Dlg, IDC_INFO_IAT, "-");
        SetDlgItemTextA(Dlg, IDC_INFO_NOTE, QemuLastFailure());
        EnableWindow(GetDlgItem(Dlg, IDC_INFO_SAVE), TRUE);
        return;
    }
    wsprintfA(Text, "Timestamp 0x%08lX, checksum 0x%08lX",
              Probe.KernelTimestamp, Probe.KernelChecksum);
    SetDlgItemTextA(Dlg, IDC_INFO_KERNEL, Text);
    SetDlgItemTextA(Dlg, IDC_INFO_DEVICE,
                    Probe.PvpanicPresent ? "Detected at I/O 0x505" :
                    "Not detected at I/O 0x505");
    if (Probe.PvpanicPresent) {
        wsprintfA(Text, "0x%02lX (shutdown %s)", Probe.PvpanicMask,
                  Probe.ShutdownSupported ? "supported" : "unavailable");
        SetDlgItemTextA(Dlg, IDC_INFO_EVENTS, Text);
    } else SetDlgItemTextA(Dlg, IDC_INFO_EVENTS, "-");
    SetDlgItemTextA(Dlg, IDC_INFO_COMPAT,
                    !Probe.Compatible ? "Unsupported" :
                    Probe.TestedKernel ? "Tested kernel" :
                    "Experimental kernel");
    if (Probe.BranchAddress)
        PutHex(Dlg, IDC_INFO_BRANCH, Probe.BranchAddress);
    else SetDlgItemTextA(Dlg, IDC_INFO_BRANCH, "-");
    if (Probe.IatAddress)
        PutHex(Dlg, IDC_INFO_IAT, Probe.IatAddress);
    else SetDlgItemTextA(Dlg, IDC_INFO_IAT, "-");
    SetDlgItemTextA(Dlg, IDC_INFO_NOTE,
                    !Probe.PvpanicPresent ?
                    "Configure QEMU pvpanic ISA at I/O port 0x505." :
                    !Probe.ShutdownSupported ?
                    "QEMU pvpanic shutdown event (0x04) is unavailable." :
                    !Probe.Compatible ? QemuProbeStatusName() :
                    State.Installed && State.Running && State.HookActive ?
                    "Shutdown hook active." : "");
}

static void ShowFailure(HWND Dlg)
{
    char Text[256];
    DWORD Error = QemuLastError();
    if (Error)
        wsprintfA(Text, "%s (0x%08lX)", QemuLastFailure(), Error);
    else
        lstrcpynA(Text, QemuLastFailure(), sizeof(Text));
    MessageBoxA(Dlg, Text, "QEMU pvpanic Driver", MB_OK | MB_ICONEXCLAMATION);
}

static void RunAction(HWND Dlg, LPCSTR Action)
{
    BOOL Ok;
    QemuSetOutput(INVALID_HANDLE_VALUE, FALSE);
    Ok = QemuAction(Action);
    if (!Ok) ShowFailure(Dlg);
    RefreshMain(Dlg);
}

static BOOL CALLBACK InfoDialog(HWND Dlg, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case WM_INITDIALOG:
        CenterDialog(Dlg);
        RefreshInfo(Dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(Wp)) {
        case IDC_INFO_REFRESH:
            QemuProbeQuick();
            RefreshInfo(Dlg);
            return TRUE;
        case IDC_INFO_SAVE:
            {
                OPENFILENAMEA Save;
                char Path[MAX_PATH];
                char InitialDir[MAX_PATH];
                static const char Filter[] = "Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
                ZeroMemory(&Save, sizeof(Save));
                lstrcpyA(Path, "QEMUPWR_DIAGNOSTICS.TXT");
                Save.lStructSize = sizeof(Save);
                Save.hwndOwner = Dlg;
                Save.lpstrFilter = Filter;
                Save.lpstrFile = Path;
                Save.nMaxFile = MAX_PATH;
                if (GetWindowsDirectoryA(InitialDir, MAX_PATH))
                    Save.lpstrInitialDir = InitialDir;
                Save.lpstrDefExt = "txt";
                Save.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameA(&Save) &&
                    !QemuExportDiagnostics(Path)) ShowFailure(Dlg);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(Dlg, 0);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(Dlg, 0);
        return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK MainDialog(HWND Dlg, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case WM_INITDIALOG:
        CenterDialog(Dlg);
        QemuProbeQuick();
        RefreshMain(Dlg);
        return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(Wp) != WA_INACTIVE) RefreshMain(Dlg);
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(Wp)) {
        case IDC_INSTALL: RunAction(Dlg, "INSTALL"); return TRUE;
        case IDC_REMOVE: RunAction(Dlg, "REMOVE"); return TRUE;
        case IDC_INFO:
            DialogBoxIndirectParamA(GetModuleHandleA(NULL),
                                    CreateInfoTemplate(), Dlg, InfoDialog, 0);
            RefreshMain(Dlg);
            return TRUE;
        case IDCANCEL: EndDialog(Dlg, 0); return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(Dlg, 0);
        return TRUE;
    }
    return FALSE;
}

extern BOOL QemupwrRunService(void);
static LPSTR GetArgs(void)
{
    LPSTR P = GetCommandLineA();
    if (*P == '"') {
        ++P;
        while (*P && *P != '"') ++P;
        if (*P) ++P;
    } else {
        while (*P && *P != ' ' && *P != '\t') ++P;
    }
    while (*P == ' ' || *P == '\t') ++P;
    return P;
}

void __cdecl WinMainCRTStartup(void)
{
    HINSTANCE Instance = GetModuleHandleA(NULL);
    LPSTR Action = GetArgs();
    QemuSetOutput(GetStdHandle(STD_OUTPUT_HANDLE), *Action != 0);
    if (lstrcmpiA(Action, "--service") == 0) {
        ExitProcess(QemupwrRunService() ? 0 : GetLastError());
    }
    if (*Action) {
        BOOL Valid = (lstrcmpiA(Action, "SCAN") == 0 ||
                      lstrcmpiA(Action, "STATUS") == 0 ||
                      lstrcmpiA(Action, "INSTALL") == 0 ||
                      lstrcmpiA(Action, "REMOVE") == 0);
        BOOL Success = Valid && QemuAction(Action);
        ExitProcess(Success ? 0 : 1);
    }
    QemuSetOutput(INVALID_HANDLE_VALUE, FALSE);
    DialogBoxIndirectParamA(Instance, CreateMainTemplate(),
                            NULL, MainDialog, 0);
    ExitProcess(0);
}
