#include <windows.h>
#include <winsvc.h>

#define AGENT_NAME "QEMUPWRAgent"
#define PATCH_TIMEOUT 30000UL
extern BOOL QpwRemapAction(LPCSTR Action);
#ifndef ERROR_TIMEOUT
#define ERROR_TIMEOUT 1460L
#endif

static SERVICE_STATUS_HANDLE g_ServiceHandle = 0;
static SERVICE_STATUS g_Status;
static HANDLE g_StopEvent = NULL;
static BOOL g_Patched = FALSE;

static void Report(DWORD State, DWORD Error, DWORD Hint)
{
    g_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState = State;
    g_Status.dwControlsAccepted = State == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    g_Status.dwWin32ExitCode = Error;
    g_Status.dwServiceSpecificExitCode = 0;
    g_Status.dwCheckPoint = State == SERVICE_START_PENDING ||
                             State == SERVICE_STOP_PENDING ? 1 : 0;
    g_Status.dwWaitHint = Hint;
    if (g_ServiceHandle != 0) SetServiceStatus(g_ServiceHandle, &g_Status);
}

static void ServiceLog(const char *Message, DWORD Code)
{
    HANDLE Source;
    LPCSTR Strings[1];
    (void)Code;
    Source = RegisterEventSourceA(NULL, AGENT_NAME);
    if (Source == NULL) return;
    Strings[0] = Message;
    ReportEventA(Source, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0, Strings, NULL);
    DeregisterEventSource(Source);
}

static VOID WINAPI ControlHandler(DWORD Code)
{
    if (Code == SERVICE_CONTROL_STOP && g_StopEvent != NULL) {
        Report(SERVICE_STOP_PENDING, NO_ERROR, PATCH_TIMEOUT + 5000UL);
        SetEvent(g_StopEvent);
    }
}

static VOID WINAPI ServiceMain(DWORD Count, LPSTR *Args)
{
    DWORD Code = 0;
    (void)Count;
    (void)Args;
    g_ServiceHandle = RegisterServiceCtrlHandlerA(AGENT_NAME, ControlHandler);
    if (g_ServiceHandle == 0) return;
    ZeroMemory(&g_Status, sizeof(g_Status));
    Report(SERVICE_START_PENDING, NO_ERROR, PATCH_TIMEOUT + 5000UL);
    g_StopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_StopEvent == NULL) {
        Code = GetLastError();
        ServiceLog("CreateEvent failed", Code);
        Report(SERVICE_STOPPED, Code, 0);
        return;
    }
    if (!QpwRemapAction("PATCH")) {
        Code = ERROR_GEN_FAILURE;
        ServiceLog("Winlogon remap failed", Code);
        Report(SERVICE_STOPPED, Code == 0 ? ERROR_GEN_FAILURE : Code, 0);
        CloseHandle(g_StopEvent);
        return;
    }
    g_Patched = TRUE;
    Report(SERVICE_RUNNING, NO_ERROR, 0);
    for (;;) {
        WaitForSingleObject(g_StopEvent, INFINITE);
        Report(SERVICE_STOP_PENDING, NO_ERROR, PATCH_TIMEOUT + 5000UL);
        if (!g_Patched || QpwRemapAction("RESTORE")) break;
        ServiceLog("Winlogon restore failed", Code);
        ResetEvent(g_StopEvent);
        Report(SERVICE_RUNNING, NO_ERROR, 0);
    }
    Report(SERVICE_STOPPED, NO_ERROR, 0);
    CloseHandle(g_StopEvent);
}

BOOL QemupwrRunService(void)
{
    SERVICE_TABLE_ENTRYA Table[2];
    Table[0].lpServiceName = AGENT_NAME;
    Table[0].lpServiceProc = ServiceMain;
    Table[1].lpServiceName = NULL;
    Table[1].lpServiceProc = 0;
    return StartServiceCtrlDispatcherA(Table);
}
