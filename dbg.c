#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <debugapi.h>
#include <winternl.h>

typedef NTSTATUS (__stdcall *pNtQueryInformationProcess)(   // We have to re-create the NTSATUS bcs we cant directly call NtQueryInformationProcess
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength
);

typedef struct _PEB_PARTIAL {   // re-create the PEB struct bcs its a remote process
    BYTE Reserved1[0x18];   // 24 bytes
    PVOID Ldr;
} PEB_PARTIAL;

typedef struct _PEB_LDR_DATA_PARTIAL {  // re-create the PEB LDR DATA bcs its a remote process
    BYTE Reserved1[8];
    PVOID Reserved2[3];
    PVOID InMemoryOrderModuleList;

} PEB_LDR_DATA_PARTIAL, *PPEB_LDR_DATA_PARTIAL;

typedef struct _LDR_DATA_TABLE_ENTRY_PARTIAL {  // re-create the LDR DATA TABLE ENTRY bcs its a remote process
    PVOID Reserved1[2]; 
    LIST_ENTRY InMemoryOrderLinks;
    PVOID Reserved2[2];
    PVOID DllBase;
    PVOID Reserved3[2];
    UNICODE_STRING FullDllName;
    BYTE Reserved4[8];
    PVOID Reserved5[3];
    __C89_NAMELESS union {
      ULONG CheckSum;
      PVOID Reserved6;
    };
    ULONG TimeDateStamp;
} LDR_DATA_TABLE_ENTRY_PARTIAL, *PLDR_DATA_TABLE_ENTRY_PARTIAL;

typedef struct _BP_ENTRY {  // struct for breakpoints, so its easier to manage
    LPVOID addr;
    BYTE original;
} BP_ENTRY;


// Get the PID of the target name

DWORD getPID(const char* target) {
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    if (Process32First(snap, &pe)) {
        do {
            if (strcmp(pe.szExeFile, target) == 0) {
                DWORD pid = pe.th32ProcessID;
                CloseHandle(snap);
                return pid;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return 0;
}


BP_ENTRY g_bp = {0};    // init the struct on BP ENTRY 
BOOL g_bp_active = FALSE;   // if there is a bp, for the single step
BOOL g_pending_single_step = FALSE; // if the single step is active

BOOL setBreakpoint(HANDLE hProcess) {
    HMODULE hUser32 = LoadLibraryA("user32.dll");
    if(!hUser32) {
        printf("Error LoadLibraryA (for user32.dll)");
        return 1;
    }

    LPVOID localAddr = (LPVOID)GetProcAddress(hUser32, "MessageBoxA");
    if(!localAddr) {
        printf("Error GetProcAddr for MessageBoxA");
        return 1;
    }

    LPVOID remoteAddr = localAddr;
    SIZE_T read;
    ReadProcessMemory(hProcess, remoteAddr, &g_bp.original, 1, &read);

    BYTE int3 = 0xCC;
    WriteProcessMemory(hProcess, remoteAddr, &int3, 1, NULL);
    FlushInstructionCache(hProcess, remoteAddr, 1);

    g_bp.addr = remoteAddr;
    g_bp_active = TRUE;
    printf("[BREAKPOINT] Set INT3 at %p (original = %p)\n", g_bp.addr, g_bp.original);
    return TRUE;
}

int main(int argc, char **argv) {

    if(argc<2) {
        printf("Usage: %s <name.exe>", argv[0]);
        return 1;
    }

    char *target = argv[1]; // name of the process to attach

    DWORD pid = getPID(target);
    if(pid == 0) {
        printf("No PID found");
        return 0;
    }
    printf("PID: %lu\n", pid);  // optionnal btw


    BOOL debug = DebugActiveProcess(pid);   // attach debug at the pid
    if(!debug) {
        printf("Error DebugActiveProcess");
        return 1;
    }
    printf("Debug Attached: %u\n", debug);

    DEBUG_EVENT event;

    HANDLE hProcess = NULL;
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if(!hNtdll) {
        printf("Error GetModuleHandleA");
        return 1;
    }

    pNtQueryInformationProcess NtQuery = (pNtQueryInformationProcess)GetProcAddress(hNtdll, "NtQueryInformationProcess");
    if(!NtQuery) {
        printf("Error NtQuery");
        return 1;
    }

    PROCESS_BASIC_INFORMATION pbi;
    HANDLE hhProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if(!hhProcess) {
        printf("Error OpenProcess");
        return 1;
    }
    NTSTATUS status = NtQuery(
        hhProcess,
        ProcessBasicInformation,
        &pbi,
        sizeof(pbi)
    );
    if(status != 0) {
        printf("Error NtQueryInformationProcess");
        CloseHandle(hhProcess);
        return 1;
    }
        printf("\n################\n##### PEB ######\n################\n\n");

    printf("PEB: %p\n", pbi.PebBaseAddress);

    PEB_PARTIAL remotePeb = {0};
    SIZE_T read = 0;
    if(!ReadProcessMemory(
        hhProcess, pbi.PebBaseAddress, &remotePeb, sizeof(remotePeb), &read) || read != sizeof(remotePeb)
    ) {
        printf("Error RPM on Remote PEB");
        CloseHandle(hhProcess);
        return 1;
    }
    printf("LDR: %p\n", remotePeb.Ldr);

    PEB_LDR_DATA_PARTIAL remoteLdrData = {0};
    if(!ReadProcessMemory(
        hhProcess, (LPCVOID)remotePeb.Ldr, &remoteLdrData, sizeof(remoteLdrData), &read) || read != sizeof(remoteLdrData)
    ) {
        printf("Error RPM on Remote LDR DATA");
        CloseHandle(hhProcess);
        return 1;
    }

    PVOID listHead = remoteLdrData.InMemoryOrderModuleList;
    LIST_ENTRY head;
    ReadProcessMemory(hhProcess, listHead, &head, sizeof(head), NULL);

    PVOID current = head.Flink;  

while (current != listHead)
{
    LIST_ENTRY entry;
    ReadProcessMemory(hhProcess, current, &entry, sizeof(entry), NULL);

    PVOID entryBase =
        (BYTE*)current - offsetof(LDR_DATA_TABLE_ENTRY_PARTIAL, InMemoryOrderLinks);

    LDR_DATA_TABLE_ENTRY_PARTIAL module = {0};
    ReadProcessMemory(hhProcess, entryBase, &module, sizeof(module), NULL);

    wchar_t nameBuf[MAX_PATH] = {0};
    ReadProcessMemory(
        hhProcess,
        module.FullDllName.Buffer,
        nameBuf,
        module.FullDllName.Length,
        NULL
    );

    wprintf(L"[MODULE] %p %ls\n", module.DllBase, nameBuf);

    current = entry.Flink;
}

    CloseHandle(hhProcess);






    printf("\n##################\n##### DEBUG ######\n##################\n\n");


    while(1) {
        if(!WaitForDebugEvent(&event, INFINITE)) {
            printf("Error WaitForDebugEvent");
            break;
        }

        switch (event.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            hProcess = event.u.CreateProcessInfo.hProcess;

            setBreakpoint(hProcess);
            LPVOID eBase = event.u.CreateProcessInfo.lpBaseOfImage;
            printf("[EVENT] CREATE PROCESS DEBUG : pid=%lu tid=%lu\n", event.dwProcessId, event.dwThreadId);
            break;

        case CREATE_THREAD_DEBUG_EVENT:
            printf("[EVENT] CREATE THREAD DEBUG : pid=%lu tid=%lu\n", event.dwProcessId, event.dwThreadId);
            break;

        case LOAD_DLL_DEBUG_EVENT:

            if(!hProcess) {
                printf("No Handle !\n");
                break;
            }
            LPVOID remotePtr = event.u.LoadDll.lpImageName;

            if (!remotePtr) {
                char path[MAX_PATH];

                if (event.u.LoadDll.hFile &&
                    GetFinalPathNameByHandleA(event.u.LoadDll.hFile, path, MAX_PATH, 0))
                {
                    printf("[DLL] (from handle file): %s\n", path);
                }
                else {
                    printf("[DLL] No DLL Image\n");
                }
                break;
            }

            LPVOID remoteStr = NULL;

            if(!ReadProcessMemory(hProcess, remotePtr, &remoteStr, sizeof(remoteStr), NULL)) {
                printf("Error RPM\n");
                break;
            }

            char nameA[MAX_PATH] = {0};
            wchar_t nameW[MAX_PATH] = {0};

            if (event.u.LoadDll.fUnicode) {
                ReadProcessMemory(hProcess, remoteStr, nameW, sizeof(nameW), NULL);
                printf("DLL: %ls\n", nameW);
            } else {
                ReadProcessMemory(hProcess, remoteStr, nameA, sizeof(nameA), NULL);
                printf("DLL: %s\n", nameA);
            }

            printf("[EVENT] LOAD DLL DEBUG : pid=%lu tid=%lu\n", event.dwProcessId, event.dwThreadId);            
            break;

        case EXCEPTION_DEBUG_EVENT:
            auto code = event.u.Exception.ExceptionRecord.ExceptionCode;
            auto addr = event.u.Exception.ExceptionRecord.ExceptionAddress;

            if(code == EXCEPTION_BREAKPOINT && addr == g_bp.addr) {
                printf("[BREAKPOINT] INT3 hit at %p\n", addr);
                HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, event.dwThreadId);


                CONTEXT ctx;
                ctx.ContextFlags = CONTEXT_ALL;
                GetThreadContext(hThread, &ctx);

                #ifdef _M_X64
                ctx.Rip -= 1;
                printf("[INFORMATION]\nRIP=%p\nRCX=%p\nRDX=%p\nR8=%p\nR9=%p\n",
                (void*)ctx.Rip, (void*)ctx.Rcx, (void*)ctx.Rdx,
                (void*)ctx.R8,  (void*)ctx.R9);
                #else
                ctx.Eip -= 1;
                #endif

                WriteProcessMemory(hProcess, g_bp.addr, &g_bp.original, 1, NULL);
                FlushInstructionCache(hProcess, g_bp.addr, 1);

                
                #ifdef _M_X64           // compilator if, if its x64 or x86
                ctx.EFlags |= 0x100;    // Trap Flag ' | ' means "dont touch to the other flags !"
                #else
                ctx.EFlags |= 0x100;    
                #endif

                SetThreadContext(hThread, &ctx);
                g_pending_single_step = TRUE;
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                break;


            }

            if (code == EXCEPTION_SINGLE_STEP && g_pending_single_step) {


                BYTE int3 = 0xCC;
                WriteProcessMemory(hProcess, g_bp.addr, &int3, 1, NULL);
                FlushInstructionCache(hProcess, g_bp.addr, 1);

                g_pending_single_step = FALSE;

                printf("[BREAKPOINT] Re-armed INT3 at %p\n", g_bp.addr);
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                break;
            }

            // Fermer le Thread !!!
            printf("[EVENT] EXCEPTION 0x%08lx at %p\n",
                code, addr);
            break;

        default:
            printf("[EVENT] Other : %lu\n", event.dwDebugEventCode);
            break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
    }

    return 0;
}