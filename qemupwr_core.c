#include <windows.h>
#include <winsvc.h>
#include "qemupwr.h"
#include "qemupwr_core.h"

#ifndef MOVEFILE_DELAY_UNTIL_REBOOT
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004UL
#endif
#ifndef ERROR_SERVICE_EXISTS
#define ERROR_SERVICE_EXISTS 1073L
#endif
#ifndef ERROR_SERVICE_DOES_NOT_EXIST
#define ERROR_SERVICE_DOES_NOT_EXIST 1060L
#endif
#ifndef ERROR_SERVICE_ALREADY_RUNNING
#define ERROR_SERVICE_ALREADY_RUNNING 1056L
#endif
#ifndef ERROR_SERVICE_NOT_ACTIVE
#define ERROR_SERVICE_NOT_ACTIVE 1062L
#endif

#define PV_SERVICE "QEMUPWR"
#define PV_DISPLAY "QEMU pvpanic Driver"
#define PV_IMAGE "\\SystemRoot\\System32\\Drivers\\QEMUPWR.SYS"
#define PV_AGENT_NAME "QEMUPWRAgent"
#define PV_AGENT_DISPLAY "QEMU pvpanic Winlogon Agent"
#define PV_AGENT_FILE "QEMUPWR.EXE"
extern BOOL QpwRemapAction(LPCSTR Action);
#define PV_AGENT_TIMEOUT 15000UL
#define PV_DEVICE "\\\\.\\QEMUPWR"

static HANDLE g_Out = INVALID_HANDLE_VALUE;
static HANDLE g_Log = INVALID_HANDLE_VALUE;
static DWORD g_FileIatSlot = 0;
static DWORD g_FileTimestamp = 0;
static DWORD g_FileChecksum = 0;
static DWORD g_FileImageSize = 0;
static DWORD g_FileCrc32 = 0;
static BOOL g_ShowConsole = TRUE;
static BOOL g_ShowFile = FALSE;
static BOOL g_QuietCleanup = FALSE;
static BOOL g_Failed = FALSE;
static LPCSTR g_Failure = NULL;
static DWORD g_FailureCode = 0;
static QEMUPWR_REPORT g_LastReport;
static BOOL g_HaveReport = FALSE;

static VOID Put(LPCSTR S)
{
    DWORD Size = (DWORD)lstrlenA(S), Written;
    if (g_ShowConsole && g_Out != INVALID_HANDLE_VALUE && g_Out != NULL)
        WriteFile(g_Out, S, Size, &Written, NULL);
    if (g_ShowFile && g_Log != INVALID_HANDLE_VALUE && g_Log != NULL)
        WriteFile(g_Log, S, Size, &Written, NULL);
}

static VOID PutHex(DWORD V)
{
    char Buf[11];
    static const char D[] = "0123456789ABCDEF";
    int I;
    Buf[0] = '0'; Buf[1] = 'x';
    for (I = 0; I < 8; ++I) Buf[I + 2] = D[(V >> (28 - 4 * I)) & 15UL];
    Buf[10] = 0;
    Put(Buf);
}

static VOID PutDec(DWORD V)
{
    char B[16];
    int I = 15;
    B[I] = 0;
    do { B[--I] = (char)('0' + (V % 10UL)); V /= 10UL; } while (V);
    Put(B + I);
}

static VOID PrintError(LPCSTR Text)
{
    g_Failed = TRUE;
    g_Failure = Text;
    g_FailureCode = GetLastError();
    Put("ERROR: "); Put(Text); Put(" Win32="); PutHex(g_FailureCode); Put("\r\n");
}

static VOID Fail(LPCSTR Text)
{
    g_Failed = TRUE;
    g_Failure = Text;
    g_FailureCode = 0;
    Put("ERROR: "); Put(Text); Put("\r\n");
}

static LPSTR Args(VOID)
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

static BOOL IsAction(LPCSTR Args, LPCSTR Action)
{
    int I = lstrlenA(Action);
    char Prefix[32];
    if (I >= sizeof(Prefix) || lstrlenA(Args) < I) return FALSE;
    CopyMemory(Prefix, Args, I);
    Prefix[I] = 0;
    if (lstrcmpiA(Prefix, Action) != 0) return FALSE;
    return Args[I] == 0 || Args[I] == ' ' || Args[I] == '\t';
}

static VOID PrintBytes(const unsigned char *D, DWORD Len)
{
    DWORD I;
    static const char H[] = "0123456789ABCDEF";
    char B[4];
    B[2] = ' '; B[3] = 0;
    for (I = 0; I < Len; ++I) {
        B[0] = H[D[I] >> 4]; B[1] = H[D[I] & 15];
        Put(B);
    }
    Put("\r\n");
}

static BOOL SourceDriverPath(LPSTR Path)
{
    DWORD Size = GetModuleFileNameA(NULL, Path, MAX_PATH);
    if (!Size || Size >= MAX_PATH - 20) {
        PrintError("GetModuleFileName");
        return FALSE;
    }
    while (Size > 0 && Path[Size - 1] != '\\') --Size;
    Path[Size] = 0;
    lstrcatA(Path, "QEMUPWR.SYS");
    return TRUE;
}

static BOOL DriverPath(LPSTR Path)
{
    DWORD Size = GetSystemDirectoryA(Path, MAX_PATH);
    if (!Size || Size > MAX_PATH - 22) {
        PrintError("GetSystemDirectory"); return FALSE;
    }
    lstrcatA(Path, "\\drivers\\QEMUPWR.SYS");
    return TRUE;
}

static BOOL DeleteDriverBinary(BOOL *Deleted)
{
    char Dest[MAX_PATH];
    DWORD Error;
    if (Deleted != NULL) *Deleted = FALSE;
    if (!DriverPath(Dest)) return FALSE;
    if (DeleteFileA(Dest)) {
        if (Deleted != NULL) *Deleted = TRUE;
        return TRUE;
    }
    Error = GetLastError();
    if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
        return TRUE;
    SetLastError(Error);
    PrintError("DeleteFile(QEMUPWR.SYS)");
    return FALSE;
}

static BOOL WaitForServiceStop(SC_HANDLE Service)
{
    SERVICE_STATUS Status;
    DWORD Attempt;
    for (Attempt = 0; Attempt < 50; ++Attempt) {
        if (!QueryServiceStatus(Service, &Status)) {
            PrintError("QueryServiceStatus");
            return FALSE;
        }
        if (Status.dwCurrentState == SERVICE_STOPPED) return TRUE;
        if (Status.dwCurrentState != SERVICE_STOP_PENDING) {
            Fail("service did not enter STOP_PENDING");
            return FALSE;
        }
        Sleep(100);
    }
    Fail("timeout waiting for service to stop");
    return FALSE;
}

static BOOL ServiceFilePath(LPSTR Path, LPCSTR Name)
{
    DWORD Size = GetSystemDirectoryA(Path, MAX_PATH);
    if (Size == 0 || Size > MAX_PATH - 24) {
        PrintError("GetSystemDirectory");
        return FALSE;
    }
    lstrcatA(Path, "\\");
    lstrcatA(Path, Name);
    return TRUE;
}

static BOOL WaitForAgent(SC_HANDLE Service, DWORD Required)
{
    SERVICE_STATUS S;
    DWORD Attempt;
    for (Attempt = 0; Attempt < 350; ++Attempt) {
        if (!QueryServiceStatus(Service, &S)) {
            PrintError("QueryServiceStatus(QEMUPWRAgent)");
            return FALSE;
        }
        if (S.dwCurrentState == Required) return TRUE;
        if (S.dwCurrentState == SERVICE_STOPPED && Required == SERVICE_RUNNING) {
            Fail("Winlogon agent failed to start");
            return FALSE;
        }
        Sleep(100);
    }
    Fail("Winlogon agent service timeout");
    return FALSE;
}

static BOOL InstalledExePath(LPSTR Path)
{
    return ServiceFilePath(Path, PV_AGENT_FILE);
}

static BOOL InstallAgent(SC_HANDLE Manager)
{
    SC_HANDLE Service;
    char Target[MAX_PATH], Source[MAX_PATH];
    char Command[MAX_PATH + 32];
    DWORD Error;
    static const char Dependency[] = "QEMUPWR\0\0";
    if (!InstalledExePath(Target)) return FALSE;
    if (!GetModuleFileNameA(NULL, Source, MAX_PATH)) {
        PrintError("GetModuleFileName");
        return FALSE;
    }
    if (lstrcmpiA(Source, Target) != 0 &&
        !CopyFileA(Source, Target, FALSE)) {
        PrintError("CopyFile(QEMUPWR.EXE)");
        return FALSE;
    }
    lstrcpyA(Command, "\"");
    lstrcatA(Command, Target);
    lstrcatA(Command, "\" --service");
    Service = CreateServiceA(Manager, PV_AGENT_NAME, PV_AGENT_DISPLAY,
                 SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                 SERVICE_CHANGE_CONFIG | DELETE,
                 SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                 SERVICE_ERROR_NORMAL, Command, NULL, NULL,
                 Dependency, NULL, NULL);
    if (Service == NULL) {
        Error = GetLastError();
        if (Error != ERROR_SERVICE_EXISTS) {
            SetLastError(Error);
            PrintError("CreateService(QEMUPWRAgent)");
            return FALSE;
        }
        Fail("existing Winlogon agent; remove previous version first");
        return FALSE;
    }
    if (!StartServiceA(Service, 0, NULL)) {
        PrintError("StartService(QEMUPWRAgent)");
        CloseServiceHandle(Service);
        return FALSE;
    }
    if (!WaitForAgent(Service, SERVICE_RUNNING)) {
        CloseServiceHandle(Service);
        return FALSE;
    }
    if (!ChangeServiceConfigA(Service, SERVICE_NO_CHANGE,
                              SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                              NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
        PrintError("ChangeServiceConfig(agent AUTO_START)");
        CloseServiceHandle(Service);
        return FALSE;
    }
    CloseServiceHandle(Service);
    Put("QEMUPWR: Winlogon remap installed\r\n");
    return TRUE;
}

static BOOL RemoveAgent(SC_HANDLE Manager)
{
    SC_HANDLE Service;
    SERVICE_STATUS S;
    DWORD Error;
    char Target[MAX_PATH];
    Service = OpenServiceA(Manager, PV_AGENT_NAME,
                           SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (Service == NULL) {
        Error = GetLastError();
        if (Error != ERROR_SERVICE_DOES_NOT_EXIST) {
            SetLastError(Error);
            PrintError("OpenService(QEMUPWRAgent)");
            return FALSE;
        }
    } else {
        if (!QueryServiceStatus(Service, &S)) {
            PrintError("QueryServiceStatus(QEMUPWRAgent)");
            CloseServiceHandle(Service);
            return FALSE;
        }
        if (S.dwCurrentState == SERVICE_RUNNING) {
            if (!ControlService(Service, SERVICE_CONTROL_STOP, &S)) {
                PrintError("ControlService(agent STOP)");
                CloseServiceHandle(Service);
                return FALSE;
            }
        }
        if (S.dwCurrentState != SERVICE_STOPPED &&
            !WaitForAgent(Service, SERVICE_STOPPED)) {
            CloseServiceHandle(Service);
            return FALSE;
        }
    }
    /* Also handles an agent that terminated before restoring Winlogon. */
    if (!QpwRemapAction("RESTORE")) {
        Fail("Winlogon remap restore failed");
        if (Service != NULL) CloseServiceHandle(Service);
        return FALSE;
    }
    if (Service != NULL) {
        if (!DeleteService(Service)) {
            PrintError("DeleteService(QEMUPWRAgent)");
            CloseServiceHandle(Service);
            return FALSE;
        }
        CloseServiceHandle(Service);
    }
    if (InstalledExePath(Target)) {
        if (!DeleteFileA(Target)) {
            Error = GetLastError();
            if (Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PATH_NOT_FOUND &&
                !MoveFileExA(Target, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
                PrintError("DeleteFile(QEMUPWR.EXE)");
                return FALSE;
            }
        }
    }
    return TRUE;
}

static BOOL StopAndDelete(SC_HANDLE Manager, BOOL RemoveDriverFile)
{
    SC_HANDLE Service;
    SERVICE_STATUS Status;
    DWORD Error;
    BOOL DeletedFile = FALSE;

    Service = OpenServiceA(Manager, PV_SERVICE,
                           SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (Service == NULL) {
        Error = GetLastError();
        if (Error == ERROR_SERVICE_DOES_NOT_EXIST) {
            if (RemoveDriverFile && !DeleteDriverBinary(&DeletedFile))
                return FALSE;
            if (!g_QuietCleanup)
                Put(DeletedFile ? "QEMUPWR driver file removed.\r\n" :
                                  "QEMUPWR: not installed\r\n");
            return TRUE;
        }
        SetLastError(Error);
        PrintError("OpenService");
        return FALSE;
    }

    if (!QueryServiceStatus(Service, &Status)) {
        PrintError("QueryServiceStatus");
        CloseServiceHandle(Service);
        return FALSE;
    }

    if (Status.dwCurrentState == SERVICE_RUNNING) {
        HANDLE Device;
        DWORD Returned;
        QEMUPWR_REPORT Report;
        Device = CreateFileA(PV_DEVICE, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Device == INVALID_HANDLE_VALUE) {
            PrintError("Open QEMUPWR device");
            CloseServiceHandle(Service);
            return FALSE;
        }
        ZeroMemory(&Report, sizeof(Report));
        if (!DeviceIoControl(Device, QEMUPWR_IOCTL_QUERY, NULL, 0,
                             &Report, sizeof(Report), &Returned, NULL) ||
            Returned != sizeof(Report) ||
            Report.Signature != QEMUPWR_SIGNATURE ||
            Report.Version != QEMUPWR_REPORT_VERSION) {
            CloseHandle(Device);
            Fail("driver version or hook state unknown");
            CloseServiceHandle(Service);
            return FALSE;
        }
        CloseHandle(Device);
        if ((Report.Flags & QEMUPWR_FLAG_ARMED) != 0) {
            Device = CreateFileA(PV_DEVICE, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (Device == INVALID_HANDLE_VALUE) {
                PrintError("Open QEMUPWR for DISARM");
                CloseServiceHandle(Service);
                return FALSE;
            }
            if (!DeviceIoControl(Device, QEMUPWR_IOCTL_DISARM,
                                 NULL, 0, NULL, 0, &Returned, NULL)) {
                CloseHandle(Device);
                PrintError("DeviceIoControl(DISARM)");
                CloseServiceHandle(Service);
                return FALSE;
            }
            CloseHandle(Device);
        }
        if (!ControlService(Service, SERVICE_CONTROL_STOP, &Status)) {
            Error = GetLastError();
            if (Error != ERROR_SERVICE_NOT_ACTIVE) {
                SetLastError(Error);
                PrintError("ControlService(STOP)");
                CloseServiceHandle(Service);
                return FALSE;
            }
        }
        if (!WaitForServiceStop(Service)) {
            CloseServiceHandle(Service);
            return FALSE;
        }
    } else if (Status.dwCurrentState == SERVICE_STOP_PENDING) {
        if (!WaitForServiceStop(Service)) {
            CloseServiceHandle(Service);
            return FALSE;
        }
    } else if (Status.dwCurrentState != SERVICE_STOPPED) {
        Fail("service is not ready for removal");
        CloseServiceHandle(Service);
        return FALSE;
    }

    if (!DeleteService(Service)) {
        PrintError("DeleteService");
        CloseServiceHandle(Service);
        return FALSE;
    }
    CloseServiceHandle(Service);

    if (RemoveDriverFile && !DeleteDriverBinary(NULL)) return FALSE;
    if (!g_QuietCleanup) Put("QEMUPWR removed.\r\n");
    return TRUE;
}

static BOOL CheckOldService(SC_HANDLE Manager)
{
    const char *Names[] = {"PVHAL6", "PVHALD", "QPWTRACE"};
    DWORD I;
    for (I = 0; I < sizeof(Names) / sizeof(Names[0]); ++I) {
        SC_HANDLE Old;
        SERVICE_STATUS S;
        Old = OpenServiceA(Manager, Names[I], SERVICE_QUERY_STATUS);
        if (Old == NULL) continue;
        if (!QueryServiceStatus(Old, &S)) {
            PrintError("QueryServiceStatus(old driver)");
            CloseServiceHandle(Old);
            return FALSE;
        }
        CloseServiceHandle(Old);
        if (S.dwCurrentState != SERVICE_STOPPED) {
            Fail("previous QEMU shutdown driver is running");
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL EnsureStarted(SC_HANDLE Manager, BOOL *Created)
{
    SC_HANDLE Service;
    DWORD Last;
    char Dest[MAX_PATH];
    SERVICE_STATUS Status;
    *Created = FALSE;
    Service = OpenServiceA(Manager, PV_SERVICE,
                           SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (Service == NULL) {
        Last = GetLastError();
        if (Last != ERROR_SERVICE_DOES_NOT_EXIST) {
            SetLastError(Last); PrintError("OpenService"); return FALSE;
        }
        if (!DriverPath(Dest)) return FALSE;
        if (GetFileAttributesA(Dest) != 0xFFFFFFFFUL) {
            Fail("QEMUPWR.SYS already exists in System32\\drivers");
            return FALSE;
        }
        {
            char Source[MAX_PATH];
            if (!SourceDriverPath(Source)) return FALSE;
            if (!CopyFileA(Source, Dest, TRUE)) {
            PrintError("CopyFile(QEMUPWR.SYS)"); return FALSE;
            }
        }
        Service = CreateServiceA(Manager, PV_SERVICE, PV_DISPLAY,
                  SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE,
                  SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                  SERVICE_ERROR_NORMAL, PV_IMAGE, NULL, NULL, NULL, NULL, NULL);
        if (Service == NULL) {
            PrintError("CreateService"); DeleteFileA(Dest); return FALSE;
        }
        *Created = TRUE;
    }
    if (!QueryServiceStatus(Service, &Status)) {
        PrintError("QueryServiceStatus"); CloseServiceHandle(Service); return FALSE;
    }
    if (Status.dwCurrentState != SERVICE_RUNNING) {
        if (!*Created) {
            Fail("service stopped; remove the previous driver before scanning");
            CloseServiceHandle(Service);
            return FALSE;
        }
        if (!StartServiceA(Service, 0, NULL)) {
            PrintError("StartService(QEMUPWR)");
            CloseServiceHandle(Service); return FALSE;
        }
    }
    CloseServiceHandle(Service);
    return TRUE;
}

static BOOL Query(HANDLE Device, QEMUPWR_REPORT *R)
{
    DWORD N;
    if (!DeviceIoControl(Device, QEMUPWR_IOCTL_QUERY, NULL, 0,
                         R, sizeof(*R), &N, NULL)) {
        PrintError("DeviceIoControl(QUERY)"); return FALSE;
    }
    if (N != sizeof(*R) || R->Signature != QEMUPWR_SIGNATURE ||
        R->Version != QEMUPWR_REPORT_VERSION || R->Size != sizeof(*R)) {
        Fail("driver/report version mismatch"); return FALSE;
    }
    g_LastReport = *R;
    g_HaveReport = TRUE;
    return TRUE;
}

static LPCSTR ResultName(DWORD Code)
{
    switch (Code) {
    case QEMUPWR_OK: return "COMPATIBLE";
    case QEMUPWR_BAD_KERNEL_BASE: return "KERNEL_BASE_UNAVAILABLE";
    case QEMUPWR_BAD_PE: return "INVALID_PE";
    case QEMUPWR_BAD_IMAGE: return "UNSUPPORTED_PE_LAYOUT";
    case QEMUPWR_NO_TEXT: return "TEXT_SECTION_NOT_AVAILABLE";
    case QEMUPWR_NO_BRANCH: return "SHUTDOWN_BRANCH_NOT_FOUND";
    case QEMUPWR_AMBIGUOUS_BRANCH: return "MULTIPLE_SHUTDOWN_BRANCH_CANDIDATES";
    case QEMUPWR_BAD_IAT: return "IMPORT_SLOT_INVALID";
    case QEMUPWR_BAD_HAL_TARGET: return "HAL_TARGET_INVALID";
    case QEMUPWR_NOT_VERIFIED: return "HAL_IMPORT_NOT_VERIFIED";
    case QEMUPWR_ALREADY_MODIFIED: return "KERNEL_PATCH_CONFLICT";
    case QEMUPWR_ALREADY_ARMED: return "ALREADY_ARMED";
    case QEMUPWR_WRITE_FAILED: return "KERNEL_PATCH_WRITE_FAILED";
    case QEMUPWR_IAT_CHANGED: return "IAT_CHANGED_DURING_INSTALL";
    case QEMUPWR_NO_PVPANIC_SHUTDOWN: return "PVPANIC_SHUTDOWN_UNAVAILABLE";
    case QEMUPWR_WINLOGON_UNSUPPORTED: return "WINLOGON_REMAP_UNVERIFIED";
    default: return "UNKNOWN_ERROR";
    }
}

static BOOL KernelImageProfileMatches(const QEMUPWR_REPORT *R)
{
    if (g_FileImageSize == R->KernelImageSize) return TRUE;
    return R->KernelTimestamp == 0x405977CDUL &&
           R->KernelChecksum == 0x000EACFFUL &&
           R->KernelImageSize == 0x000DC000UL &&
           g_FileImageSize == 0x000E5000UL &&
           g_FileCrc32 == 0x55287965UL &&
           R->BranchMatches == 1UL &&
           R->BranchAddress == 0x80158297UL &&
           R->IatAddress == 0x80144818UL;
}

static BOOL FileImportMatches(const QEMUPWR_REPORT *R)
{
    return g_FileIatSlot != 0 &&
           g_FileIatSlot == R->IatAddress &&
           g_FileTimestamp == R->KernelTimestamp &&
           g_FileChecksum == R->KernelChecksum &&
           KernelImageProfileMatches(R);
}

static BOOL TestedKernelFile(const QEMUPWR_REPORT *R)
{
    if (!(R->Flags & QEMUPWR_FLAG_KNOWN)) return FALSE;
    if (R->KernelTimestamp == 0x321A03D2UL &&
        R->KernelChecksum == 0x000D3B29UL &&
        R->KernelImageSize == 0x000BCD00UL &&
        g_FileCrc32 == 0x409E1420UL) return TRUE;
    if (R->KernelTimestamp == 0x405977CDUL &&
        R->KernelChecksum == 0x000EACFFUL &&
        (R->KernelImageSize == 0x000E5000UL ||
         R->KernelImageSize == 0x000DC000UL) &&
        g_FileCrc32 == 0x55287965UL &&
        R->BranchMatches == 1UL &&
        R->BranchAddress == 0x80158297UL &&
        R->IatAddress == 0x80144818UL) return TRUE;
    return FALSE;
}

static VOID PrintReport(const QEMUPWR_REPORT *R)
{
    Put("\r\nKernel runtime scan\r\n-------------------\r\n");
    Put("Status: ");
    if (R->Result != QEMUPWR_OK) Put(ResultName(R->Result));
    else if (!FileImportMatches(R)) Put("KERNEL_IMPORT_MISMATCH");
    else Put("COMPATIBLE");
    Put("\r\n");
    Put("Flags: "); PutHex(R->Flags); Put("\r\n");
    Put("Tested kernel file: ");
    Put(TestedKernelFile(R) ? "YES\r\n" : "NO\r\n");
    Put("Runtime import metadata: ");
    Put((R->Flags & QEMUPWR_FLAG_IMPORT) ? "AVAILABLE\r\n" : "UNAVAILABLE\r\n");
    Put("Hook active: ");
    Put((R->Flags & QEMUPWR_FLAG_ARMED) ? "YES\r\n" : "NO\r\n");
    Put("File import accepted for experimental install: ");
    Put((R->Flags & QEMUPWR_FLAG_FILE_IMPORT) ? "YES\r\n" : "NO\r\n");
    Put("Kernel runtime base: "); PutHex(R->KernelBase); Put("\r\n");
    Put("Kernel PE image base: "); PutHex(R->KernelPreferredBase); Put("\r\n");
    Put("Kernel PE timestamp: "); PutHex(R->KernelTimestamp); Put("\r\n");
    Put("Kernel PE checksum: "); PutHex(R->KernelChecksum); Put("\r\n");
    Put("Kernel PE size: "); PutHex(R->KernelImageSize); Put("\r\n");
    Put("Matched code section RVA: "); PutHex(R->KernelTextRva); Put("\r\n");
    Put("Matched code section size: "); PutHex(R->KernelTextSize); Put("\r\n");
    Put("Branch candidates: "); PutDec(R->BranchMatches); Put("\r\n");
    Put("Shutdown path address: "); PutHex(R->BranchAddress); Put("\r\n");
    Put("HAL call address: "); PutHex(R->CallAddress); Put("\r\n");
    Put("HAL import slot: "); PutHex(R->IatAddress); Put("\r\n");
    Put("HAL runtime target: "); PutHex(R->HalTarget); Put("\r\n");
    Put("Pvpanic capability mask: "); PutHex(R->PvpanicMask); Put("\r\n");
    if (R->Flags & QEMUPWR_FLAG_IMPORT) {
        Put("Runtime HAL import slot: "); PutHex(R->HalImportFromTable); Put("\r\n");
        Put("HAL import call candidates: "); PutDec(R->HalCallCandidates); Put("\r\n");
        Put("First HAL import CALL: "); PutHex(R->FirstHalCall); Put("\r\n");
        Put("First HAL CALL context: ");
        PrintBytes(R->HalCallContext, sizeof(R->HalCallContext));
    }
    Put("Branch context (8 bytes preceding, 40 following): ");
    PrintBytes(R->BranchContext, sizeof(R->BranchContext));
    Put("IAT context (8 bytes preceding, 8 following): ");
    PrintBytes(R->IatContext, sizeof(R->IatContext));
    Put("Kernel file HAL import: ");
    if (g_FileIatSlot) PutHex(g_FileIatSlot);
    else Put("unavailable");
    Put("\r\n");
    if (g_FileIatSlot) {
        Put("Kernel file/runtime IAT: ");
        Put(FileImportMatches(R) ? "MATCH\r\n" : "MISMATCH\r\n");
    }

}

static DWORD File32(const BYTE *P)
{
    return (DWORD)P[0] | ((DWORD)P[1] << 8) |
           ((DWORD)P[2] << 16) | ((DWORD)P[3] << 24);
}

static DWORD File16(const BYTE *P)
{
    return (DWORD)P[0] | ((DWORD)P[1] << 8);
}

static const BYTE *FileRva(const BYTE *Data, DWORD Length,
                           DWORD Pe, DWORD Rva, DWORD Count)
{
    DWORD OptLen, SectionCount, Sec, I, Va, VSize, RawSize, Raw, Delta;
    if (Pe > Length - 24) return NULL;
    SectionCount = File16(Data + Pe + 6);
    OptLen = File16(Data + Pe + 20);
    if (SectionCount == 0 || SectionCount > 48 ||
        OptLen < 0xE0 || Pe + 24 + OptLen > Length) return NULL;
    Sec = Pe + 24 + OptLen;
    if (SectionCount * 40 > Length - Sec) return NULL;
    if (Rva < File32(Data + Pe + 24 + 60)) {
        if (Rva <= Length && Count <= Length - Rva) return Data + Rva;
        return NULL;
    }
    for (I = 0; I < SectionCount; ++I) {
        const BYTE *S = Data + Sec + I * 40;
        VSize = File32(S + 8);
        Va = File32(S + 12);
        RawSize = File32(S + 16);
        Raw = File32(S + 20);
        if (Rva < Va || Rva - Va >= VSize) continue;
        Delta = Rva - Va;
        if (Delta > RawSize || Count > RawSize - Delta ||
            Raw > Length || Delta > Length - Raw ||
            Count > Length - Raw - Delta) return NULL;
        return Data + Raw + Delta;
    }
    return NULL;
}

static BOOL FileNameIs(const BYTE *Data, DWORD Length,
                       DWORD Pe, DWORD Rva, LPCSTR Name)
{
    DWORD I;
    const BYTE *P;
    DWORD N = (DWORD)lstrlenA(Name) + 1;
    P = FileRva(Data, Length, Pe, Rva, N);
    if (P == NULL) return FALSE;
    for (I = 0; I < N; ++I) {
        BYTE C = P[I], E = (BYTE)Name[I];
        if (C >= 'A' && C <= 'Z') C += 'a' - 'A';
        if (E >= 'A' && E <= 'Z') E += 'a' - 'A';
        if (C != E) return FALSE;
    }
    return TRUE;
}

static VOID FindFileImport(LPCSTR Path)
{
    HANDLE H;
    DWORD Length, Read, Pe, Opt, ImportRva, ImportLen;
    DWORD I, J, DescRva, Oft, First, Name, Thunk;
    BYTE *Data;
    const BYTE *P;
    g_FileIatSlot = 0;
    H = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (H == INVALID_HANDLE_VALUE) return;
    Length = GetFileSize(H, NULL);
    if (Length < 0x200 || Length > 0x400000UL) {
        CloseHandle(H); return;
    }
    Data = (BYTE *)GlobalAlloc(GMEM_FIXED, Length);
    if (Data == NULL) { CloseHandle(H); return; }
    if (!ReadFile(H, Data, Length, &Read, NULL) || Read != Length)
        goto Done;
    if (Data[0] != 'M' || Data[1] != 'Z') goto Done;
    Pe = File32(Data + 0x3C);
    if (Pe > Length - 0x120 || File32(Data + Pe) != 0x4550) goto Done;
    Opt = Pe + 24;
    if (File16(Data + Pe + 20) < 0xE0 ||
        File16(Data + Opt) != 0x10B) goto Done;
    g_FileTimestamp = File32(Data + Pe + 8);
    g_FileChecksum = File32(Data + Opt + 64);
    g_FileImageSize = File32(Data + Opt + 56);
    ImportRva = File32(Data + Opt + 104);
    ImportLen = File32(Data + Opt + 108);
    if (ImportLen < 20 || ImportLen > 0x10000UL) goto Done;
    for (I = 0; I < ImportLen / 20 && I < 128; ++I) {
        DescRva = ImportRva + I * 20;
        P = FileRva(Data, Length, Pe, DescRva, 20);
        if (P == NULL) break;
        Oft = File32(P);
        Name = File32(P + 12);
        First = File32(P + 16);
        if (Oft == 0 && First == 0) break;
        if (Oft == 0 || First == 0 ||
            !FileNameIs(Data, Length, Pe, Name, "HAL.dll")) continue;
        for (J = 0; J < 512; ++J) {
            P = FileRva(Data, Length, Pe, Oft + J * 4, 4);
            if (P == NULL) break;
            Thunk = File32(P);
            if (Thunk == 0) break;
            if ((Thunk & 0x80000000UL) != 0) continue;
            if (FileNameIs(Data, Length, Pe, Thunk + 2,
                           "HalReturnToFirmware")) {
                g_FileIatSlot = File32(Data + Opt + 28) + First + J * 4;
                break;
            }
        }
    }
Done:
    GlobalFree((HGLOBAL)Data);
    CloseHandle(H);
}

static VOID PrintFileInfo(LPCSTR Label, LPCSTR Filename)
{
    HANDLE File;
    DWORD Read, Size, Crc = 0xFFFFFFFFUL, I, J;
    BYTE Buffer[4096], Head[1024];
    DWORD Pos = 0;
    char Path[MAX_PATH];
    DWORD Dir;

    Dir = GetSystemDirectoryA(Path, MAX_PATH);
    if (!Dir || Dir > MAX_PATH - 16) return;
    lstrcatA(Path, Filename);
    Put(Label); Put(" file: "); Put(Path); Put("\r\n");
    File = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        PrintError("CreateFile(kernel or HAL)"); return;
    }
    Size = GetFileSize(File, NULL);
    ZeroMemory(Head, sizeof(Head));
    do {
        if (!ReadFile(File, Buffer, sizeof(Buffer), &Read, NULL)) {
            PrintError("ReadFile(kernel or HAL)"); break;
        }
        if (Read == 0) break;
        for (I = 0; I < Read; ++I) {
            if (Pos < sizeof(Head)) Head[Pos] = Buffer[I];
            ++Pos;
            Crc ^= Buffer[I];
            for (J = 0; J < 8; ++J)
                Crc = (Crc >> 1) ^ ((Crc & 1UL) ? 0xEDB88320UL : 0UL);
        }
    } while (Read != 0);
    CloseHandle(File);
    if (lstrcmpiA(Label, "Kernel") == 0) FindFileImport(Path);
    Crc ^= 0xFFFFFFFFUL;
    if (lstrcmpiA(Label, "Kernel") == 0) g_FileCrc32 = Crc;
    Put(Label); Put(" file bytes: "); PutDec(Size); Put("\r\n");
    Put(Label); Put(" file CRC32: "); PutHex(Crc); Put("\r\n");
    if (Head[0] == 'M' && Head[1] == 'Z') {
        DWORD Hdr = *(DWORD *)(Head + 0x3C);
        if (Hdr < sizeof(Head) - 0x100 &&
            Head[Hdr] == 'P' && Head[Hdr + 1] == 'E' &&
            Head[Hdr + 2] == 0 && Head[Hdr + 3] == 0) {
            Put(Label); Put(" file timestamp: ");
            PutHex(*(DWORD *)(Head + Hdr + 8)); Put("\r\n");
            Put(Label); Put(" file checksum: ");
            PutHex(*(DWORD *)(Head + Hdr + 24 + 64)); Put("\r\n");
            Put(Label); Put(" file image size: ");
            PutHex(*(DWORD *)(Head + Hdr + 24 + 56)); Put("\r\n");
        }
    }
}

static VOID PrintOs(VOID)
{
    OSVERSIONINFOA V;
    HKEY Key;
    char Text[128];
    DWORD Length, Type;
    ZeroMemory(&V, sizeof(V));
    V.dwOSVersionInfoSize = sizeof(V);
    Put("OS / build\r\n----------\r\n");
    if (GetVersionExA(&V)) {
        Put("Major: "); PutDec(V.dwMajorVersion); Put("\r\n");
        Put("Minor: "); PutDec(V.dwMinorVersion); Put("\r\n");
        Put("Build: "); PutDec(V.dwBuildNumber & 0xFFFFUL); Put("\r\n");
        Put("CSD: "); Put(V.szCSDVersion); Put("\r\n");
        Put("Platform ID: "); PutDec(V.dwPlatformId); Put("\r\n");
    } else PrintError("GetVersionEx");
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS) {
        Length = sizeof(Text) - 1; Type = 0;
        if (RegQueryValueExA(Key, "CSDVersion", NULL, &Type,
                             (LPBYTE)Text, &Length) == ERROR_SUCCESS &&
            Type == REG_SZ) {
            Text[Length < sizeof(Text) ? Length : sizeof(Text) - 1] = 0;
            Put("Registry CSDVersion: "); Put(Text); Put("\r\n");
        }
        RegCloseKey(Key);
    }
    PrintFileInfo("Kernel", "\\ntoskrnl.exe");
    PrintFileInfo("HAL", "\\hal.dll");
}

static VOID PrintCompactOs(VOID)
{
    OSVERSIONINFOA V;
    ZeroMemory(&V, sizeof(V));
    V.dwOSVersionInfoSize = sizeof(V);
    if (!GetVersionExA(&V)) return;
    Put("QEMUPWR: Windows NT ");
    PutDec(V.dwMajorVersion);
    Put(".");
    PutDec(V.dwMinorVersion);
    if (V.szCSDVersion[0] != 0) {
        Put(" ");
        Put(V.szCSDVersion);
    }
    Put("\r\n");
}

static BOOL Diagnostics(LPCSTR Action, LPCSTR Path)
{
    BOOL OldConsole = g_ShowConsole;
    BOOL OldFile = g_ShowFile;
    g_Log = CreateFileA(Path, GENERIC_WRITE,
                        FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Log == INVALID_HANDLE_VALUE) {
        PrintError("CreateFile(report)");
        return FALSE;
    }
    g_ShowConsole = FALSE;
    g_ShowFile = TRUE;
    Put("QEMU pvpanic compatibility report\r\n");
    Put("==========================\r\n");
    Put("Action: "); Put(Action); Put("\r\n\r\n");
    PrintOs();
    if (g_HaveReport) PrintReport(&g_LastReport);
    if (g_Failure != NULL) {
        Put("\r\nFailure: "); Put(g_Failure);
        if (g_FailureCode != 0) {
            Put(" Win32="); PutHex(g_FailureCode);
        }
        Put("\r\n");
    }
    CloseHandle(g_Log);
    g_Log = INVALID_HANDLE_VALUE;
    g_ShowConsole = OldConsole;
    g_ShowFile = OldFile;
    return TRUE;
}

static BOOL Run(LPCSTR Action)
{
    BOOL IsScan = IsAction(Action, "SCAN");
    BOOL IsInstall = IsAction(Action, "INSTALL");
    BOOL IsStatus = IsAction(Action, "STATUS");
    BOOL IsRemove = IsAction(Action, "REMOVE");
    BOOL Experimental = lstrcmpiA(Action, "INSTALL EXPERIMENTAL") == 0;
    SC_HANDLE Manager;
    SC_HANDLE Service;
    SERVICE_STATUS ServiceState;
    HANDLE Device = INVALID_HANDLE_VALUE;
    BOOL Created = FALSE;
    BOOL Ok = FALSE;
    BOOL Armed = FALSE;
    DWORD Returned;
    DWORD Control;
    QEMUPWR_REPORT Report;
    QEMUPWR_FILE_IMPORT Verify;
    HKEY AutoArmKey;
    DWORD AutoArm = 1;
    DWORD RegError;

    Manager = OpenSCManagerA(NULL, NULL,
              IsScan || IsInstall || IsRemove ?
              (SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE) : SC_MANAGER_CONNECT);
    if (Manager == NULL) { PrintError("OpenSCManager"); return FALSE; }

    if (IsRemove) {
        Ok = RemoveAgent(Manager);
        if (Ok) Ok = StopAndDelete(Manager, TRUE);
        CloseServiceHandle(Manager);
        return Ok;
    }

    if (IsStatus) {
        Service = OpenServiceA(Manager, PV_SERVICE,
                               SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
        if (Service == NULL) {
            if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
                Put("QEMUPWR: not installed\r\n");
                CloseServiceHandle(Manager);
                return TRUE;
            }
            PrintError("OpenService");
            CloseServiceHandle(Manager);
            return FALSE;
        }
        if (!QueryServiceStatus(Service, &ServiceState)) {
            PrintError("QueryServiceStatus");
            CloseServiceHandle(Service);
            CloseServiceHandle(Manager);
            return FALSE;
        }
        {
            BYTE ConfigBuf[1024];
            QUERY_SERVICE_CONFIGA *Config = (QUERY_SERVICE_CONFIGA *)ConfigBuf;
            DWORD Needed = 0;
            if (QueryServiceConfigA(Service, Config, sizeof(ConfigBuf), &Needed)) {
                Put("QEMUPWR: startup ");
                Put(Config->dwStartType == SERVICE_AUTO_START ? "Automatic" : "Manual");
                Put("\r\n");
            }
        }
        CloseServiceHandle(Service);
        {
            SC_HANDLE Agent = OpenServiceA(Manager, PV_AGENT_NAME,
                                           SERVICE_QUERY_STATUS);
            SERVICE_STATUS AgentStatus;
            if (Agent != NULL) {
                if (QueryServiceStatus(Agent, &AgentStatus)) {
                    Put("QEMUPWR: Winlogon agent ");
                    Put(AgentStatus.dwCurrentState == SERVICE_RUNNING ?
                        "running\r\n" : "stopped\r\n");
                }
                CloseServiceHandle(Agent);
            }
        }
        if (ServiceState.dwCurrentState != SERVICE_RUNNING) {
            Put("QEMUPWR: service stopped\r\n");
            CloseServiceHandle(Manager);
            return TRUE;
        }
    } else {
        if (!CheckOldService(Manager)) goto Cleanup;
        if (!EnsureStarted(Manager, &Created)) goto Cleanup;
    }

    Device = CreateFileA(PV_DEVICE, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Device == INVALID_HANDLE_VALUE) {
        PrintError("CreateFile(QEMUPWR device)");
        goto Cleanup;
    }
    if (!Query(Device, &Report)) goto Cleanup;

    if (IsScan) {
        PrintCompactOs();
        Put("QEMUPWR: "); Put(ResultName(Report.Result)); Put("\r\n");
        if (Report.BranchAddress != 0) {
            Put("QEMUPWR: Shutdown path ");
            PutHex(Report.BranchAddress); Put("\r\n");
        }
        if (Report.Result != QEMUPWR_OK)
            Fail(ResultName(Report.Result));
        else if (!FileImportMatches(&Report))
            Fail("kernel file/runtime import mismatch");
        else Ok = TRUE;
        goto Cleanup;
    }
    if (IsStatus) {
        PrintCompactOs();
        Put("QEMUPWR: driver running; hook ");
        Put((Report.Flags & QEMUPWR_FLAG_ARMED) ? "active" : "inactive");
        Put("\r\n");
        Put("QEMUPWR: pvpanic="); PutHex(Report.PvpanicMask);
        Put("\r\n");
        Ok = TRUE;
        goto Cleanup;
    }

    if (Report.Result != QEMUPWR_OK &&
        !(Experimental && Report.Result == QEMUPWR_NOT_VERIFIED)) {
        Fail(ResultName(Report.Result));
        goto Cleanup;
    }
    if (!FileImportMatches(&Report)) {
        Fail("kernel file/runtime import mismatch");
        goto Cleanup;
    }
    if (Experimental && Report.Result == QEMUPWR_NOT_VERIFIED) {
        Verify.Size = sizeof(Verify);
        Verify.KernelTimestamp = g_FileTimestamp;
        Verify.KernelChecksum = g_FileChecksum;
        Verify.KernelImageSize = g_FileImageSize;
        Verify.HalIatAddress = g_FileIatSlot;
        if (!DeviceIoControl(Device, QEMUPWR_IOCTL_VERIFY_FILE,
                             &Verify, sizeof(Verify), NULL, 0,
                             &Returned, NULL)) {
            PrintError("DeviceIoControl(VERIFY_FILE)");
            Query(Device, &Report);
            goto Cleanup;
        }
        if (!Query(Device, &Report)) goto Cleanup;
    }
    if (!TestedKernelFile(&Report) && !Experimental) {
        Fail("unsupported kernel profile");
        goto Cleanup;
    }
    if (Report.PvpanicMask == 0xFFUL ||
        (Report.PvpanicMask & 0x04UL) == 0) {
        Fail("pvpanic shutdown (0x04) unavailable");
        goto Cleanup;
    }
    Armed = (Report.Flags & QEMUPWR_FLAG_ARMED) != 0;
    if (!Armed) {
        Control = Experimental ? QEMUPWR_IOCTL_ARM_EXPERIMENTAL : QEMUPWR_IOCTL_ARM;
        if (!DeviceIoControl(Device, Control, NULL, 0,
                             NULL, 0, &Returned, NULL)) {
            PrintError("DeviceIoControl(ARM)");
            Query(Device, &Report);
            goto Cleanup;
        }
    }
    if (!Query(Device, &Report)) goto Cleanup;
    if ((Report.Flags & QEMUPWR_FLAG_ARMED) == 0) {
        Fail("hook inactive after installation");
        goto Cleanup;
    }
    if (!Experimental && TestedKernelFile(&Report)) {
        if (!InstallAgent(Manager)) goto Cleanup;
        Service = OpenServiceA(Manager, PV_SERVICE, SERVICE_CHANGE_CONFIG);
        if (Service == NULL) {
            PrintError("OpenService(CHANGE_CONFIG)");
            goto Cleanup;
        }
        RegError = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Services\\QEMUPWR",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
            NULL, &AutoArmKey, NULL);
        if (RegError != ERROR_SUCCESS) {
            SetLastError(RegError);
            PrintError("RegCreateKeyEx(QEMUPWR)");
            CloseServiceHandle(Service);
            goto Cleanup;
        }
        RegError = RegSetValueExA(AutoArmKey, "AutoArm", 0, REG_DWORD,
                                  (const BYTE *)&AutoArm, sizeof(AutoArm));
        if (RegError != ERROR_SUCCESS) {
            SetLastError(RegError);
            PrintError("RegSetValueEx(AutoArm)");
            RegCloseKey(AutoArmKey);
            CloseServiceHandle(Service);
            goto Cleanup;
        }
        RegCloseKey(AutoArmKey);
        if (!ChangeServiceConfigA(Service, SERVICE_NO_CHANGE,
                SERVICE_AUTO_START, SERVICE_NO_CHANGE, NULL, NULL,
                NULL, NULL, NULL, NULL, NULL)) {
            PrintError("ChangeServiceConfig(AUTO_START)");
            CloseServiceHandle(Service);
            goto Cleanup;
        }
        CloseServiceHandle(Service);
    }
    PrintCompactOs();
    Put("QEMUPWR: Shutdown path "); PutHex(Report.BranchAddress);
    Put("\r\n");
    Put(Armed ? "QEMUPWR: hook already active\r\n" :
                "QEMUPWR: hook installed\r\n");
    Ok = TRUE;

Cleanup:
    if (Device != INVALID_HANDLE_VALUE) CloseHandle(Device);
    if (IsScan && Created) {
        g_QuietCleanup = TRUE;
        if (!StopAndDelete(Manager, TRUE)) Ok = FALSE;
        g_QuietCleanup = FALSE;
    }
    if (IsInstall && !Ok && Created) {
        g_QuietCleanup = TRUE;
        if (RemoveAgent(Manager)) StopAndDelete(Manager, TRUE);
        g_QuietCleanup = FALSE;
    }
    CloseServiceHandle(Manager);
    return Ok;
}


void QemuPreflight(void)
{
    BOOL Console = g_ShowConsole, File = g_ShowFile;
    g_FileIatSlot = 0;
    g_FileCrc32 = 0;
    g_ShowConsole = FALSE;
    g_ShowFile = FALSE;
    PrintOs();
    g_ShowConsole = Console;
    g_ShowFile = File;
}

void QemuSetOutput(HANDLE Output, BOOL Console)
{
    g_Out = Output;
    g_ShowConsole = Console;
    g_ShowFile = FALSE;
}

BOOL QemuAction(LPCSTR Action)
{
    BOOL Scan = lstrcmpiA(Action, "SCAN") == 0;
    BOOL Install = IsAction(Action, "INSTALL");
    BOOL Ok;
    g_Failed = FALSE;
    g_Failure = NULL;
    g_FailureCode = 0;
    g_HaveReport = FALSE;
    if (Scan || Install) QemuPreflight();
    Ok = Run(Action);
    if (Scan) Put("SCAN complete.\r\n");
    return Ok && !g_Failed;
}

DWORD QemuLastError(void) { return g_FailureCode; }
LPCSTR QemuLastFailure(void) { return g_Failure ? g_Failure : "Operation failed"; }

BOOL QemuProbeQuick(void)
{
    QEMU_STATE State;
    BOOL Ok;
    g_Failed = FALSE;
    g_Failure = NULL;
    g_FailureCode = 0;
    g_HaveReport = FALSE;
    if (!QemuGetState(&State)) {
        Fail("service status unavailable");
        return FALSE;
    }
    if (State.Installed && !State.Running) {
        Fail("service stopped");
        return FALSE;
    }
    QemuPreflight();
    Ok = Run("SCAN");
    return Ok && !g_Failed;
}

BOOL QemuGetProbe(QEMU_PROBE *Probe)
{
    QEMUPWR_REPORT *Report;
    ZeroMemory(Probe, sizeof(*Probe));
    if (!g_HaveReport) return FALSE;
    Report = &g_LastReport;
    Probe->Available = TRUE;
    Probe->Result = Report->Result;
    Probe->Compatible = Report->Result == QEMUPWR_OK &&
                        FileImportMatches(Report);
    Probe->TestedKernel = TestedKernelFile(Report);
    Probe->ImportMatched = FileImportMatches(Report);
    Probe->PvpanicMask = Report->PvpanicMask;
    Probe->PvpanicPresent = Report->PvpanicMask != 0xFFUL;
    Probe->ShutdownSupported = Probe->PvpanicPresent &&
                                (Report->PvpanicMask & 4UL) != 0;
    Probe->HookActive = (Report->Flags & QEMUPWR_FLAG_ARMED) != 0;
    Probe->KernelTimestamp = Report->KernelTimestamp;
    Probe->KernelChecksum = Report->KernelChecksum;
    Probe->BranchAddress = Report->BranchAddress;
    Probe->IatAddress = Report->IatAddress;
    Probe->HalTarget = Report->HalTarget;
    return TRUE;
}

LPCSTR QemuProbeStatusName(void)
{
    if (!g_HaveReport) return "Scan unavailable";
    if (g_LastReport.Result != QEMUPWR_OK)
        return ResultName(g_LastReport.Result);
    if (!FileImportMatches(&g_LastReport))
        return "KERNEL_IMPORT_MISMATCH";
    return "COMPATIBLE";
}

BOOL QemuExportDiagnostics(LPCSTR Path)
{
    return Diagnostics("SCAN", Path);
}

BOOL QemuGetState(QEMU_STATE *State)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS ServiceStatus;
    QUERY_SERVICE_CONFIGA *Config;
    BYTE Buffer[1024];
    DWORD Needed;
    HANDLE Device;
    QEMUPWR_REPORT Report;
    ZeroMemory(State, sizeof(*State));
    Manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (Manager == NULL) return FALSE;
    Service = OpenServiceA(Manager, PV_SERVICE,
                           SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (Service == NULL) {
        DWORD Error = GetLastError();
        CloseServiceHandle(Manager);
        return Error == ERROR_SERVICE_DOES_NOT_EXIST;
    }
    State->Installed = TRUE;
    if (QueryServiceStatus(Service, &ServiceStatus))
        State->Running = ServiceStatus.dwCurrentState == SERVICE_RUNNING;
    Config = (QUERY_SERVICE_CONFIGA *)Buffer;
    if (QueryServiceConfigA(Service, Config, sizeof(Buffer), &Needed))
        State->Automatic = Config->dwStartType == SERVICE_AUTO_START;
    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
    {
        SC_HANDLE AgentManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
        SC_HANDLE Agent = AgentManager == NULL ? NULL :
                          OpenServiceA(AgentManager, PV_AGENT_NAME,
                                       SERVICE_QUERY_STATUS);
        SERVICE_STATUS AgentStatus;
        if (Agent != NULL) {
            State->AgentInstalled = TRUE;
            if (QueryServiceStatus(Agent, &AgentStatus))
                State->AgentRunning = AgentStatus.dwCurrentState == SERVICE_RUNNING;
            CloseServiceHandle(Agent);
        }
        if (AgentManager != NULL) CloseServiceHandle(AgentManager);
    }
    if (!State->Running) return TRUE;
    Device = CreateFileA(PV_DEVICE, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Device == INVALID_HANDLE_VALUE) return TRUE;
    if (Query(Device, &Report)) {
        State->DriverResponding = TRUE;
        State->HookActive = (Report.Flags & QEMUPWR_FLAG_ARMED) != 0;
        State->Compatible = Report.Result == QEMUPWR_OK;
        State->PvpanicAvailable = Report.PvpanicMask != 0xFFUL &&
                                  (Report.PvpanicMask & 4UL) != 0;
        State->PvpanicMask = Report.PvpanicMask;
        State->BranchAddress = Report.BranchAddress;
    }
    CloseHandle(Device);
    return TRUE;
}

LPCSTR QemuCommandLineArgs(void)
{
    return Args();
}

