/*
CAPE - Config And Payload Extraction
Copyright(C) 2019 kevoreilly@gmail.com

This program is free software : you can redistribute it and / or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <distorm.h>
#include "..\hooking.h"
#include "Debugger.h"
#include "CAPE.h"
#include <psapi.h>

#define MAX_INSTRUCTIONS 0x10
#define SINGLE_STEP_LIMIT 0x4000  // default unless specified in web ui
#define CHUNKSIZE 0x10 * MAX_INSTRUCTIONS
#define RVA_LIMIT 0x200000
#define DoClearZeroFlag 1
#define DoSetZeroFlag   2
#define PrintEAX        3

extern void DoOutputDebugString(_In_ LPCTSTR lpOutputString, ...);
extern void DoOutputErrorString(_In_ LPCTSTR lpOutputString, ...);
extern void DebuggerOutput(_In_ LPCTSTR lpOutputString, ...);
extern int DumpImageInCurrentProcess(LPVOID ImageBase);
extern int DumpMemory(LPVOID Buffer, SIZE_T Size);
extern char *convert_address_to_dll_name_and_offset(ULONG_PTR addr, unsigned int *offset);
extern BOOL is_in_dll_range(ULONG_PTR addr);
extern DWORD_PTR FileOffsetToVA(DWORD_PTR modBase, DWORD_PTR dwOffset);
extern DWORD_PTR GetEntryPointVA(DWORD_PTR modBase);
extern BOOL ScyllaGetSectionByName(PVOID ImageBase, char* Name, PVOID* SectionData, SIZE_T* SectionSize);
extern PCHAR ScyllaGetExportNameByAddress(PVOID Address, PCHAR* ModuleName);
extern ULONG_PTR g_our_dll_base;
extern BOOL inside_hook(LPVOID Address);

char *ModuleName, *PreviousModuleName;
BOOL BreakpointsSet, BreakpointsHit, FilterTrace, TraceAll, StopTrace;
PVOID ModuleBase, DumpAddress;
BOOL GetSystemTimeAsFileTimeImported, PayloadMarker, PayloadDumped, TraceRunning;
unsigned int DumpCount, Correction, StepCount, StepLimit, TraceDepthLimit;
char Action0[MAX_PATH], Action1[MAX_PATH], Action2[MAX_PATH], Action3[MAX_PATH], *Instruction0, *Instruction1, *Instruction2, *Instruction3;
unsigned int Type0, Type1, Type2, Type3;
int StepOverRegister, TraceDepthCount, EntryPointRegister;
CONTEXT LastContext;
SIZE_T DumpSize, LastWriteLength;
char DumpSizeString[MAX_PATH], DebuggerBuffer[MAX_PATH];

BOOL Trace(struct _EXCEPTION_POINTERS* ExceptionInfo);
BOOL BreakpointCallback(PBREAKPOINTINFO pBreakpointInfo, struct _EXCEPTION_POINTERS* ExceptionInfo);

BOOL DoSetSingleStepMode(int Register, PCONTEXT Context, PVOID Handler)
{
    GetNextAvailableBreakpoint(GetCurrentThreadId(), &StepOverRegister);
    if (!StepOverRegister)
        StepOverRegister = Register;
    return SetSingleStepMode(Context, Trace);
}

void ActionDispatch()
{
	return;
}

BOOL Trace(struct _EXCEPTION_POINTERS* ExceptionInfo)
{
	PVOID ReturnAddress, CIP;
    BOOL StepOver, ForceStepOver;
    unsigned int DllRVA;
#ifdef BRANCH_TRACE
    PVOID BranchTarget;
#endif

    TraceRunning = TRUE;

    _DecodeType DecodeType;
    _DecodeResult Result;
    _OffsetType Offset = 0;
    _DecodedInst DecodedInstruction;
    unsigned int DecodedInstructionsCount = 0;

#ifdef _WIN64
    CIP = (PVOID)ExceptionInfo->ContextRecord->Rip;
    DecodeType = Decode64Bits;
#else
    CIP = (PVOID)ExceptionInfo->ContextRecord->Eip;
    DecodeType = Decode32Bits;
#endif

    //if (!is_in_dll_range((ULONG_PTR)CIP) || TraceAll || InsideHook(NULL, CIP) || g_config.trace_into_api[0])
    if (!is_in_dll_range((ULONG_PTR)CIP) || TraceAll || g_config.break_on_apiname)
    {
        FilterTrace = FALSE;
        StepCount++;
    }
    else if (InsideHook(NULL, CIP) || inside_hook(CIP) || is_in_dll_range((ULONG_PTR)CIP))
        FilterTrace = TRUE;

    //MODULEINFO ModuleInfo;
    //HMODULE TraceModule = GetModuleHandle("SHELL32");
    //if (TraceModule && GetModuleInformation(GetCurrentProcess(), TraceModule, &ModuleInfo, sizeof(MODULEINFO)))
    //{
    //    if ((ULONG_PTR)CIP >= (ULONG_PTR)TraceModule && (ULONG_PTR)CIP < ((ULONG_PTR)TraceModule + ModuleInfo.SizeOfImage))
    //        FilterTrace = FALSE;
    //}
#ifdef _WIN64
    if (!FilterTrace && LastContext.Rip)
    {
        memset(DebuggerBuffer, 0, MAX_PATH*sizeof(CHAR));

        if (LastContext.Rax != ExceptionInfo->ContextRecord->Rax)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RAX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rax);

        if (LastContext.Rbx != ExceptionInfo->ContextRecord->Rbx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RBX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rbx);

        if (LastContext.Rcx != ExceptionInfo->ContextRecord->Rcx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RCX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rcx);

        if (LastContext.Rdx != ExceptionInfo->ContextRecord->Rdx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RDX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rdx);

        if (LastContext.Rsi != ExceptionInfo->ContextRecord->Rsi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RSI=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rsi);

        if (LastContext.Rdi != ExceptionInfo->ContextRecord->Rdi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RDI=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rdi);

        if (LastContext.Rsp != ExceptionInfo->ContextRecord->Rsp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RSP=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rsp);

        if (LastContext.Rbp != ExceptionInfo->ContextRecord->Rbp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RBP=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rbp);
#else
    if (!FilterTrace && LastContext.Eip)
    {
        memset(DebuggerBuffer, 0, MAX_PATH*sizeof(CHAR));

        if (LastContext.Eax != ExceptionInfo->ContextRecord->Eax)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EAX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Eax);

        if (LastContext.Ebx != ExceptionInfo->ContextRecord->Ebx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EBX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Ebx);

        if (LastContext.Ecx != ExceptionInfo->ContextRecord->Ecx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s ECX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Ecx);

        if (LastContext.Edx != ExceptionInfo->ContextRecord->Edx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EDX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Edx);

        if (LastContext.Esi != ExceptionInfo->ContextRecord->Esi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s ESI=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Esi);

        if (LastContext.Edi != ExceptionInfo->ContextRecord->Edi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EDI=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Edi);

        if (LastContext.Esp != ExceptionInfo->ContextRecord->Esp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s ESP=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Esp);

        if (LastContext.Ebp != ExceptionInfo->ContextRecord->Ebp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EBP=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Ebp);
#endif

        DebuggerOutput(DebuggerBuffer);
    }

    if (!FilterTrace)
        DebuggerOutput("\n");

    if (StepCount > StepLimit)
    {
        DebuggerOutput("Single-step limit reached (%d), releasing.\n", StepLimit);
        StepCount = 0;
        return TRUE;
    }

    ModuleName = convert_address_to_dll_name_and_offset((ULONG_PTR)CIP, &DllRVA);
    PCHAR FunctionName;

    if (ModuleName)
    {
        if (!PreviousModuleName || strncmp(ModuleName, PreviousModuleName, strlen(ModuleName)))
        {
            __try
            {
                FunctionName = ScyllaGetExportNameByAddress(CIP, NULL);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                DoOutputDebugString("Trace: Error dereferencing instruction pointer 0x%p.\n", CIP);
            }
            if (FilterTrace)
                DebuggerOutput("\n");
            if (FunctionName)
            {
                DebuggerOutput("Break in %s::%s (RVA 0x%x, thread %d)\n", ModuleName, FunctionName, DllRVA, GetCurrentThreadId());
                for (unsigned int i = 0; i < ARRAYSIZE(g_config.trace_into_api); i++)
                {
                    if (!g_config.trace_into_api[i])
                        break;
                    if (!stricmp(FunctionName, g_config.trace_into_api[i]))
                        StepOver = FALSE;
                }
            }
            else
                DebuggerOutput("Break in %s (RVA 0x%x, thread %d)\n", ModuleName, DllRVA, GetCurrentThreadId());
            if (PreviousModuleName)
                free (PreviousModuleName);
            PreviousModuleName = ModuleName;
        }
    }

#ifdef BRANCH_TRACE
    if (ExceptionInfo->ExceptionRecord->ExceptionInformation[0])
    {
        BranchTarget = CIP;
        CIP = (PVOID)ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
    }
#endif
    if (CIP)
        Result = distorm_decode(Offset, (const unsigned char*)CIP, CHUNKSIZE, DecodeType, &DecodedInstruction, 1, &DecodedInstructionsCount);

    if (!strcmp(DecodedInstruction.mnemonic.p, "CALL"))
    {
        PCHAR ExportName;
        StepOver = FALSE;
        ForceStepOver = FALSE;

        if (FilterTrace && !TraceAll)
            StepOver = TRUE;
        else if (DecodedInstruction.size > 4 && DecodedInstruction.operands.length && !strncmp(DecodedInstruction.operands.p, "DWORD", 5) && strncmp(DecodedInstruction.operands.p, "DWORD [E", 8))
        {
            PVOID CallTarget = *(PVOID*)((PUCHAR)CIP + DecodedInstruction.size - 4);
            __try
            {
                ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                DoOutputDebugString("Trace: Error dereferencing CallTarget 0x%x.\n", CallTarget);
                ExportName = NULL;
            }

            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else if (!strncmp(DecodedInstruction.operands.p, "DWORD [0x", 9))
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
            }
            else if (!strncmp(DecodedInstruction.operands.p, "DWORD [FS:0xc0]", 15))
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
                ForceStepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
        }
        else if (DecodedInstruction.size > 4)
        {
            PVOID CallTarget = (PVOID)((PUCHAR)CIP + (int)*(DWORD*)((PUCHAR)CIP + DecodedInstruction.size - 4) + DecodedInstruction.size);
            __try
            {
                ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                DoOutputDebugString("Trace: Error dereferencing CallTarget 0x%x.", CallTarget);
                ExportName = NULL;
            }

            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
        }
        else if (!strncmp(DecodedInstruction.operands.p, "EAX", 3))
        {
#ifdef _WIN64
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Rax;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#else
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Eax;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#endif
        }
        else if (!strncmp(DecodedInstruction.operands.p, "EBX", 3))
        {
#ifdef _WIN64
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Rbx;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#else
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Ebx;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#endif
        }
        else if (!strncmp(DecodedInstruction.operands.p, "ECX", 3))
        {
#ifdef _WIN64
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Rcx;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#else
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Ecx;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#endif
        }
        else if (!strncmp(DecodedInstruction.operands.p, "EDX", 3))
        {
#ifdef _WIN64
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Rdx;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s%-30s", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#else
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Edx;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#endif
        }
        else if (!strncmp(DecodedInstruction.operands.p, "EBP", 3))
        {
#ifdef _WIN64
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Rbp;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%p (%02d) %-20s %-6s%-4s0x%-24p", ExceptionInfo->ContextRecord->Rip, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#else
            PVOID CallTarget = (PVOID)ExceptionInfo->ContextRecord->Ebp;
            ExportName = ScyllaGetExportNameByAddress(CallTarget, NULL);
            if (ExportName)
            {
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", ExportName);
                StepOver = TRUE;
            }
            else
                DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s0x%-28x", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", CallTarget);
#endif
        }
        else
            DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
#ifdef BRANCH_TRACE
        if (!FilterTrace)
            TraceDepthCount++;
#else
        if (ExportName)
        {
            for (unsigned int i = 0; i < ARRAYSIZE(g_config.trace_into_api); i++)
            {
                if (!g_config.trace_into_api[i])
                    break;
                if (!stricmp(ExportName, g_config.trace_into_api[i]))
                {
                    StepOver = FALSE;
                    TraceDepthCount--;
                    DebuggerOutput("\nTrace: Stepping into %s\n", ExportName);
                }
            }
        }

        if (((unsigned int)abs(TraceDepthCount) >= TraceDepthLimit && !TraceAll) || (StepOver == TRUE && !TraceAll) || ForceStepOver)
        {
            ReturnAddress = (PVOID)((PUCHAR)CIP + DecodedInstruction.size);
            if (!ContextSetThreadBreakpoint(ExceptionInfo->ContextRecord, StepOverRegister, 0, (BYTE*)ReturnAddress, BP_EXEC, BreakpointCallback))
                DoOutputDebugString("Trace: Failed to set breakpoint on return address 0x%p\n", ReturnAddress);

            if (ForceStepOver)
                DoOutputDebugString("Trace: Set breakpoint on return address 0x%p\n", ReturnAddress);

            LastContext = *ExceptionInfo->ContextRecord;

            ClearSingleStepMode(ExceptionInfo->ContextRecord);

            return TRUE;
        }
        else
            TraceDepthCount++;
#endif
    }
    else if (!strcmp(DecodedInstruction.mnemonic.p, "RET"))
    {
#ifdef BRANCH_TRACE
        if (!FilterTrace)
            TraceDepthCount--;
#else
        if (!FilterTrace || TraceAll)
            DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
        //if ((unsigned int)abs(TraceDepthCount) >= TraceDepthLimit)
        //{
        //    DebuggerOutput("Trace: Stepping out of initial depth, releasing.");
        //    ClearSingleStepMode(ExceptionInfo->ContextRecord);
        //    return TRUE;
        //}

        TraceDepthCount--;
#endif
    }
#ifndef _WIN64
    else if (!strcmp(DecodedInstruction.mnemonic.p, "CALL FAR") && !strncmp(DecodedInstruction.operands.p, "0x33", 4))
    {
        ReturnAddress = (PVOID)((PUCHAR)CIP + DecodedInstruction.size);
        if (!ContextSetThreadBreakpoint(ExceptionInfo->ContextRecord, StepOverRegister, 0, (BYTE*)ReturnAddress, BP_EXEC, BreakpointCallback))
            DoOutputDebugString("Trace: Failed to set breakpoint on Heaven's Gate return address 0x%p\n", ReturnAddress);

        LastContext = *ExceptionInfo->ContextRecord;

        ClearSingleStepMode(ExceptionInfo->ContextRecord);

        if (!FilterTrace)
            DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", (unsigned int)CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
        return TRUE;
    }
#endif
    else if (!FilterTrace)
#ifdef _WIN64
        DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
#else
        DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", (unsigned int)CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);
#endif

    if (!stricmp(Action0, "dumpebx"))
    {
        if (!stricmp(DumpSizeString, "eax"))
        {
#ifdef _WIN64
            DumpSize = ExceptionInfo->ContextRecord->Rax;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Rbx;
#else
            DumpSize = ExceptionInfo->ContextRecord->Eax;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Ebx;
#endif
            if (g_config.dumptype0)
                CapeMetaData->DumpType = g_config.dumptype0;
            else
                CapeMetaData->DumpType = EXTRACTION_PE;

            if (DumpMemory(DumpAddress, DumpSize))
            {
                DoOutputDebugString("Trace: Dumped config region at 0x%p size 0x%x.\n", DumpAddress, DumpSize);
                return TRUE;
            }
            else
                DoOutputDebugString("Trace: Failed to dump config region at 0x%p.\n", DumpAddress);
        }
    }

    if (Instruction0 && !stricmp(DecodedInstruction.mnemonic.p, Instruction0))
    {
        if (!stricmp(Action0, "ClearZeroFlag"))
        {
            ClearZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, clearing zero flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "SetZeroFlag"))
        {
            SetZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, setting zero flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "FlipZeroFlag"))
        {
            FlipZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, flipping zero flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "ClearSignFlag"))
        {
            ClearSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, clearing Sign flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "SetSignFlag"))
        {
            SetSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, setting Sign flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "FlipSignFlag"))
        {
            FlipSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, flipping Sign flag (action0).\n", Instruction0);
        }
#ifndef _WIN64
        else if (!stricmp(Action0, "PrintEAX"))
        {
            if (ExceptionInfo->ContextRecord->Eax)
                DebuggerOutput("Trace: Print EAX -> 0x%x.\n", ExceptionInfo->ContextRecord->Eax);
        }
#endif
        else
            DebuggerOutput("Trace: Unrecognised action0! (%s)\n", Action0);

        Instruction0 = NULL;
    }

    if (Instruction1 && !stricmp(DecodedInstruction.mnemonic.p, Instruction1))
    {
        if (!stricmp(Action1, "ClearZeroFlag"))
        {
            ClearZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, clearing zero flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "SetZeroFlag"))
        {
            SetZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nTrace: %s detected, setting zero flag (action1).\n", Instruction1);
        }

        Instruction1 = NULL;
    }

    LastContext = *ExceptionInfo->ContextRecord;

    if (!StopTrace)
        SetSingleStepMode(ExceptionInfo->ContextRecord, Trace);

    TraceRunning = FALSE;

    return TRUE;
}

BOOL StepOutCallback(PBREAKPOINTINFO pBreakpointInfo, struct _EXCEPTION_POINTERS* ExceptionInfo)
{
	PVOID DumpAddress, CIP;
    _DecodeType DecodeType;
    _DecodeResult Result;
    _OffsetType Offset = 0;
    _DecodedInst DecodedInstruction;
    unsigned DecodedInstructionsCount = 0;

	if (pBreakpointInfo == NULL)
	{
		DoOutputDebugString("StepOutCallback executed with pBreakpointInfo NULL.\n");
		return FALSE;
	}

	if (pBreakpointInfo->ThreadHandle == NULL)
	{
		DoOutputDebugString("StepOutCallback executed with NULL thread handle.\n");
		return FALSE;
	}

#ifdef _WIN64
    CIP = (PVOID)ExceptionInfo->ContextRecord->Rip;
    DecodeType = Decode64Bits;
#else
    CIP = (PVOID)ExceptionInfo->ContextRecord->Eip;
    DecodeType = Decode32Bits;
#endif

    if (!is_in_dll_range((ULONG_PTR)CIP) || TraceAll || g_config.break_on_apiname)
        FilterTrace = FALSE;
    else if (InsideHook(NULL, CIP) || is_in_dll_range((ULONG_PTR)CIP))
        FilterTrace = TRUE;

    DebuggerOutput("StepOutCallback: Breakpoint hit by instruction at 0x%p\n", CIP);

    Result = distorm_decode(Offset, (const unsigned char*)CIP, CHUNKSIZE, DecodeType, &DecodedInstruction, 1, &DecodedInstructionsCount);
    DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);

    if (!stricmp(Action0, "dumpebx"))
    {
        if (!stricmp(DumpSizeString, "eax"))
        {
#ifdef _WIN64
            DumpSize = ExceptionInfo->ContextRecord->Rax;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Rbx;
#else
            DumpSize = ExceptionInfo->ContextRecord->Eax;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Ebx;
#endif
            if (g_config.dumptype0)
                CapeMetaData->DumpType = g_config.dumptype0;
            else
                CapeMetaData->DumpType = EXTRACTION_PE;

            if (DumpMemory(DumpAddress, DumpSize))
                DoOutputDebugString("StepOutCallback: Dumped config region at 0x%p size 0x%x.\n", DumpAddress, DumpSize);
            else
                DoOutputDebugString("StepOutCallback: Failed to dump config region at 0x%p.\n", DumpAddress);
        }
    }

    StepOverExecutionBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo);

    return TRUE;
}

BOOL BreakpointCallback(PBREAKPOINTINFO pBreakpointInfo, struct _EXCEPTION_POINTERS* ExceptionInfo)
{
	PVOID ReturnAddress, CIP;
    _DecodeType DecodeType;
    _DecodeResult Result;
    _OffsetType Offset = 0;
    _DecodedInst DecodedInstruction;
    unsigned int DllRVA, bp, DecodedInstructionsCount = 0;

    if (pBreakpointInfo == NULL)
	{
		DoOutputDebugString("BreakpointCallback executed with pBreakpointInfo NULL.\n");
		return FALSE;
	}

	if (pBreakpointInfo->ThreadHandle == NULL)
	{
		DoOutputDebugString("BreakpointCallback executed with NULL thread handle.\n");
		return FALSE;
	}

    BreakpointsHit = TRUE;

    for (bp = 0; bp < NUMBER_OF_DEBUG_REGISTERS; bp++)
    {
        if (pBreakpointInfo->Register == bp)
        {
            TraceDepthCount = 0;
            if (bp == 0 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr0))
                DoOutputDebugString("BreakpointCallback: Breakpoint 0 hit at 0x%p\n", pBreakpointInfo->Address);

            if (bp == 1 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr1))
                DoOutputDebugString("BreakpointCallback: Breakpoint 1 hit at 0x%p\n", pBreakpointInfo->Address);

            if (bp == 2 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr2))
                DoOutputDebugString("BreakpointCallback: Breakpoint 2 hit at 0x%p\n", pBreakpointInfo->Address);

            if (bp == 3 && ((DWORD_PTR)pBreakpointInfo->Address != ExceptionInfo->ContextRecord->Dr3))
                DoOutputDebugString("BreakpointCallback: Breakpoint 3 hit at 0x%p\n", pBreakpointInfo->Address);
        }
    }

#ifdef _WIN64
    CIP = (PVOID)ExceptionInfo->ContextRecord->Rip;
    DecodeType = Decode64Bits;
#else
    CIP = (PVOID)ExceptionInfo->ContextRecord->Eip;
    DecodeType = Decode32Bits;
#endif
    if (!is_in_dll_range((ULONG_PTR)CIP) || TraceAll || g_config.break_on_apiname)
        FilterTrace = FALSE;
    else if (InsideHook(NULL, CIP) || is_in_dll_range((ULONG_PTR)CIP))
        FilterTrace = TRUE;

    //if (CIP == bp0 || CIP == bp1 || CIP == bp2 || CIP == bp3)
    //if (!TraceRunning)
    //    DebuggerOutput("BreakpointCallback: Breakpoint hit by instruction at 0x%p\n", CIP);

    //MODULEINFO ModuleInfo;
    //HMODULE TraceModule = GetModuleHandle("SHELL32");
    //if (TraceModule && GetModuleInformation(GetCurrentProcess(), TraceModule, &ModuleInfo, sizeof(MODULEINFO)))
    //{
    //    if ((ULONG_PTR)CIP >= (ULONG_PTR)TraceModule && (ULONG_PTR)CIP < ((ULONG_PTR)TraceModule + ModuleInfo.SizeOfImage))
    //        FilterTrace = FALSE;
    //}

#ifdef _WIN64
    if (!FilterTrace && LastContext.Rip)
    {
        memset(DebuggerBuffer, 0, MAX_PATH*sizeof(CHAR));

        if (LastContext.Rax != ExceptionInfo->ContextRecord->Rax)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RAX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rax);

        if (LastContext.Rbx != ExceptionInfo->ContextRecord->Rbx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RBX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rbx);

        if (LastContext.Rcx != ExceptionInfo->ContextRecord->Rcx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RCX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rcx);

        if (LastContext.Rdx != ExceptionInfo->ContextRecord->Rdx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RDX=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rdx);

        if (LastContext.Rsi != ExceptionInfo->ContextRecord->Rsi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RSI=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rsi);

        if (LastContext.Rdi != ExceptionInfo->ContextRecord->Rdi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RDI=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rdi);

        if (LastContext.Rsp != ExceptionInfo->ContextRecord->Rsp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RSP=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rsp);

        if (LastContext.Rbp != ExceptionInfo->ContextRecord->Rbp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s RBP=%#I64x", DebuggerBuffer, ExceptionInfo->ContextRecord->Rbp);
#else
    if (!FilterTrace && LastContext.Eip)
    {
        memset(DebuggerBuffer, 0, MAX_PATH*sizeof(CHAR));

        if (LastContext.Eax != ExceptionInfo->ContextRecord->Eax)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EAX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Eax);

        if (LastContext.Ebx != ExceptionInfo->ContextRecord->Ebx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EBX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Ebx);

        if (LastContext.Ecx != ExceptionInfo->ContextRecord->Ecx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s ECX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Ecx);

        if (LastContext.Edx != ExceptionInfo->ContextRecord->Edx)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EDX=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Edx);

        if (LastContext.Esi != ExceptionInfo->ContextRecord->Esi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s ESI=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Esi);

        if (LastContext.Edi != ExceptionInfo->ContextRecord->Edi)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EDI=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Edi);

        if (LastContext.Esp != ExceptionInfo->ContextRecord->Esp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s ESP=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Esp);

        if (LastContext.Ebp != ExceptionInfo->ContextRecord->Ebp)
            _snprintf_s(DebuggerBuffer, MAX_PATH, _TRUNCATE, "%s EBP=0x%x", DebuggerBuffer, ExceptionInfo->ContextRecord->Ebp);
#endif

        DebuggerOutput(DebuggerBuffer);
    }

    if (!FilterTrace)
        DebuggerOutput("\n");

    ModuleName = convert_address_to_dll_name_and_offset((ULONG_PTR)CIP, &DllRVA);

    if (ModuleName)
    {
        if (!PreviousModuleName || strncmp(ModuleName, PreviousModuleName, strlen(ModuleName)))
        {
            PCHAR FunctionName;

            __try
            {
                FunctionName = ScyllaGetExportNameByAddress(CIP, NULL);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                DoOutputDebugString("BreakpointCallback: Error dereferencing instruction pointer 0x%p.\n", CIP);
            }
            if (FilterTrace)
                DebuggerOutput("\n");
            if (FunctionName)
                DebuggerOutput("Break at 0x%p in %s::%s (RVA 0x%x, thread %d)\n", CIP, ModuleName, FunctionName, DllRVA, GetCurrentThreadId());
            else
                DebuggerOutput("Break at 0x%p in %s (RVA 0x%x, thread %d)\n", CIP, ModuleName, DllRVA, GetCurrentThreadId());
            if (PreviousModuleName)
                free (PreviousModuleName);
            PreviousModuleName = ModuleName;
        }
    }

//    if (g_config.step_out)
//    {
//        BYTE* BreakpointAddress;
//        int Register = 0;
//        g_config.step_out = 0;
//
//#ifdef _WIN64
//        BreakpointAddress = (BYTE*)*(DWORD_PTR*)((BYTE*)ExceptionInfo->ContextRecord->Rbp+8);
//#else
//        BreakpointAddress = (BYTE*)*(DWORD*)((BYTE*) ExceptionInfo->ContextRecord->Ebp+4);
//#endif
//        if (ContextSetThreadBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, 0, BreakpointAddress, BP_EXEC, StepOutCallback))
//            DoOutputDebugString("BreakpointCallback: Breakpoint %d set on ret instruction at 0x%p.\n", Register, BreakpointAddress);
//        else
//            DoOutputDebugString("BreakpointCallback: Failed to set breakpoint %d on ret instruction at 0x%p.\n", Register, BreakpointAddress);
//
//        return TRUE;
//    }
    if (g_config.step_out)
    {
        unsigned int Register = 1, Delta = 0, ChunkSize = 0x300, MaxInstructions = 0x100;    // Size of code to disassemble to search for ret
        _DecodedInst DecodedInstructions[0x100];

        memset(&DecodedInstructions, 0, sizeof(DecodedInstructions));
        Result = distorm_decode(Offset, (const unsigned char*)CIP, ChunkSize, DecodeType, DecodedInstructions, MaxInstructions, &DecodedInstructionsCount);

#ifdef _WIN64
        DoOutputDebugString("BreakpointCallback: Searching for ret instruction to step-out from 0x%p (0x%x instructions) (0x%p).\n", CIP, DecodedInstructionsCount, *(DWORD_PTR*)((BYTE*) ExceptionInfo->ContextRecord->Rbp+8));
#else
        DoOutputDebugString("BreakpointCallback: Searching for ret instruction to step-out from 0x%p (0x%x instructions) (0x%x).\n", CIP, DecodedInstructionsCount, *(DWORD*)((BYTE*) ExceptionInfo->ContextRecord->Ebp+4));
#endif
        g_config.step_out = 0;

        for (unsigned int i = 0; i < DecodedInstructionsCount; i++)
        {
            if (!strcmp(DecodedInstructions[i].mnemonic.p, "RET"))
            {
                if (ContextSetThreadBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, 0, (BYTE*)CIP + Delta, BP_EXEC, StepOutCallback))
                    DoOutputDebugString("BreakpointCallback: Breakpoint %d set on ret instruction at 0x%p.\n", Register, (BYTE*)CIP + Delta);
                else
                    DoOutputDebugString("BreakpointCallback: Failed to set breakpoint %d on ret instruction at 0x%p.\n", Register, (BYTE*)CIP + Delta);
                break;
            }

            Delta += DecodedInstructions[i].size;
        }

        return TRUE;
    }

    Result = distorm_decode(Offset, (const unsigned char*)CIP, CHUNKSIZE, DecodeType, &DecodedInstruction, 1, &DecodedInstructionsCount);
    if (!FilterTrace)
        DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);

    if (!strcmp(DecodedInstruction.mnemonic.p, "CALL"))
    {
#ifdef BRANCH_TRACE
        if (!FilterTrace)
            TraceDepthCount++;
#else
        if ((unsigned int)abs(TraceDepthCount) >= TraceDepthLimit && !TraceAll)
        {
            ReturnAddress = (PVOID)((PUCHAR)CIP + DecodedInstruction.size);
            if (!ContextSetThreadBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo->Register, 0, (BYTE*)ReturnAddress, BP_EXEC, BreakpointCallback))
            {
                DoOutputDebugString("BreakpointCallback: Failed to set breakpoint on return address 0x%p\n", ReturnAddress);
            }

            LastContext = *ExceptionInfo->ContextRecord;

            StepOverExecutionBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo);

            return TRUE;
        }
        else if (!FilterTrace)
            TraceDepthCount++;
#endif
}
    else if (!strcmp(DecodedInstruction.mnemonic.p, "RET"))
    {
        //if (TraceDepthCount < 0)
        //{
        //    DebuggerOutput("BreakpointCallback: Stepping out of initial depth, releasing.\n");
        //    StepOverExecutionBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo);
        //    return TRUE;
        //}
        //else if (!FilterTrace)
        if (!FilterTrace)
            TraceDepthCount--;
    }

    if (!stricmp(Action0, "dumpebx"))
    {
        if (!stricmp(DumpSizeString, "eax"))
        {
            PVOID CallingModule = GetAllocationBase(CIP);
#ifdef _WIN64
            DumpSize = ExceptionInfo->ContextRecord->Rax;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Rbx;
#else
            DumpSize = ExceptionInfo->ContextRecord->Eax;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Ebx;
#endif
            if (g_config.dumptype0)
                CapeMetaData->DumpType = g_config.dumptype0;
            else
                CapeMetaData->DumpType = EXTRACTION_PE;

            if (DumpMemory(DumpAddress, DumpSize))
                DoOutputDebugString("BreakpointCallback: Dumped config region at 0x%p size 0x%x.\n", DumpAddress, DumpSize);
            else
                DoOutputDebugString("BreakpointCallback: Failed to dump config region at 0x%p.\n", DumpAddress);

            //if (g_config.region_type)
            //{
            //    CapeMetaData->DumpType = g_config.region_type;
            //    if (DumpPEsInRange(CallingModule, 1))
            //        DoOutputDebugString("BreakpointCallback: Dumped breaking module at 0x%p.\n", CallingModule);
            //    else
            //        DoOutputDebugString("BreakpointCallback: Failed to dump breaking module at 0x%p.\n", CallingModule);
            //}

            StepOverExecutionBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo);
            return TRUE;
        }
    }

    if (!stricmp(Action0, "dumpedx"))
    {
        if (!stricmp(DumpSizeString, "ecx"))
        {
            PVOID CallingModule = GetAllocationBase(CIP);
#ifdef _WIN64
            DumpSize = ExceptionInfo->ContextRecord->Rcx;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Rdx;
#else
            DumpSize = ExceptionInfo->ContextRecord->Ecx;
            DumpAddress = (PVOID)ExceptionInfo->ContextRecord->Edx;
#endif
            if (g_config.dumptype0)
                CapeMetaData->DumpType = g_config.dumptype0;
            else
                CapeMetaData->DumpType = EXTRACTION_PE;

            if (DumpMemory(DumpAddress, DumpSize))
                DoOutputDebugString("BreakpointCallback: Dumped config region at 0x%p size 0x%x.\n", DumpAddress, DumpSize);
            else
                DoOutputDebugString("BreakpointCallback: Failed to dump config region at 0x%p.\n", DumpAddress);

            StepOverExecutionBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo);
            return TRUE;
        }
    }

    if (Instruction0 && !stricmp(DecodedInstruction.mnemonic.p, Instruction0))
    {
        if (!stricmp(Action0, "ClearZeroFlag"))
        {
            ClearZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, clearing zero flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "SetZeroFlag"))
        {
            SetZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, setting zero flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "FlipZeroFlag"))
        {
            FlipZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, flipping zero flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "ClearSignFlag"))
        {
            ClearSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, clearing Sign flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "SetSignFlag"))
        {
            SetSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, setting Sign flag (action0).\n", Instruction0);
        }
        else if (!stricmp(Action0, "FlipSignFlag"))
        {
            FlipSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, flipping Sign flag (action0).\n", Instruction0);
        }
#ifndef _WIN64
        else if (!stricmp(Action0, "PrintEAX"))
        {
            if (ExceptionInfo->ContextRecord->Eax)
                DebuggerOutput("\nBreakpointCallback: Print EAX -> 0x%x.", ExceptionInfo->ContextRecord->Eax);
        }
#endif
        else if (!stricmp(Action0, "Dump"))
        {
            PVOID CallingModule = GetAllocationBase(CIP);
            if (g_config.dumptype0)
                CapeMetaData->DumpType = g_config.dumptype0;
            else
                CapeMetaData->DumpType = EXTRACTION_PE;

            if (DumpImageInCurrentProcess(CallingModule))
                DoOutputDebugString("BreakpointCallback: Dumped breaking module at 0x%p.\n", CallingModule);
            else
                DoOutputDebugString("BreakpointCallback: Failed to dump breaking module at 0x%p.\n", CallingModule);
        }
        else
            DebuggerOutput("\nBreakpointCallback: Unrecognised action0! (%s)", Action0);

        Instruction0 = NULL;
    }

    if (Instruction1 && !stricmp(DecodedInstruction.mnemonic.p, Instruction1))
    {
        if (!stricmp(Action1, "ClearZeroFlag"))
        {
            ClearZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, clearing zero flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "SetZeroFlag"))
        {
            SetZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, setting zero flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "FlipZeroFlag"))
        {
            FlipZeroFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, flipping zero flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "ClearSignFlag"))
        {
            ClearSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, clearing Sign flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "SetSignFlag"))
        {
            SetSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, setting Sign flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "FlipSignFlag"))
        {
            FlipSignFlag(ExceptionInfo->ContextRecord);
            DebuggerOutput("\nBreakpointCallback: %s detected, flipping Sign flag (action1).\n", Instruction1);
        }
        else if (!stricmp(Action1, "Dump"))
        {
            PVOID CallingModule = GetAllocationBase(CIP);
            if (g_config.dumptype0)
                CapeMetaData->DumpType = g_config.dumptype0;
            else
                CapeMetaData->DumpType = EXTRACTION_PE;

            if (DumpImageInCurrentProcess(CallingModule))
                DoOutputDebugString("BreakpointCallback: Dumped breaking module at 0x%p.\n", CallingModule);
            else
                DoOutputDebugString("BreakpointCallback: Failed to dump breaking module at 0x%p.\n", CallingModule);
        }
#ifndef _WIN64
        else if (!stricmp(Action1, "PrintEAX"))
        {
            if (ExceptionInfo->ContextRecord->Eax)
                DebuggerOutput("\nBreakpointCallback: Print EAX -> 0x%x.", ExceptionInfo->ContextRecord->Eax);
        }
#endif
        else
            DebuggerOutput("\nBreakpointCallback: Unrecognised action1! (%s)", Action1);

        Instruction1 = NULL;
    }

    LastContext = *ExceptionInfo->ContextRecord;

    StepOverExecutionBreakpoint(ExceptionInfo->ContextRecord, pBreakpointInfo);

    if (!StopTrace)
        DoSetSingleStepMode(pBreakpointInfo->Register, ExceptionInfo->ContextRecord, Trace);

    return TRUE;
}

BOOL WriteCallback(PBREAKPOINTINFO pBreakpointInfo, struct _EXCEPTION_POINTERS* ExceptionInfo)
{
	PVOID CIP;
    _DecodeType DecodeType;
    _DecodeResult Result;
    _OffsetType Offset = 0;
    _DecodedInst DecodedInstruction;
    unsigned int DecodedInstructionsCount = 0;
    char OutputBuffer[MAX_PATH] = "";

	if (pBreakpointInfo == NULL)
	{
		DoOutputDebugString("WriteCallback executed with pBreakpointInfo NULL.\n");
		return FALSE;
	}

	if (pBreakpointInfo->ThreadHandle == NULL)
	{
		DoOutputDebugString("WriteCallback executed with NULL thread handle.\n");
		return FALSE;
	}

    BreakpointsHit = TRUE;

#ifdef _WIN64
    CIP = (PVOID)ExceptionInfo->ContextRecord->Rip;
    DecodeType = Decode64Bits;
#else
    CIP = (PVOID)ExceptionInfo->ContextRecord->Eip;
    DecodeType = Decode32Bits;
#endif

    Result = distorm_decode(Offset, (const unsigned char*)CIP, CHUNKSIZE, DecodeType, &DecodedInstruction, 1, &DecodedInstructionsCount);

    DebuggerOutput("0x%x (%02d) %-20s %-6s%-4s%-30s", CIP, DecodedInstruction.size, (char*)DecodedInstruction.instructionHex.p, (char*)DecodedInstruction.mnemonic.p, DecodedInstruction.operands.length != 0 ? " " : "", (char*)DecodedInstruction.operands.p);

    return TRUE;
}

BOOL BreakpointOnReturn(PVOID Address)
{
    unsigned int Register;
    if (!SetNextAvailableBreakpoint(GetCurrentThreadId(), &Register, 0, Address, BP_EXEC, BreakpointCallback))
    {
        DoOutputDebugString("BreakpointOnReturn: failed to set breakpoint.\n");
        return FALSE;
    }
    // TODO: add option to break once only, clearing bp
    //strncpy(g_config.break_on_return, "\0", 2);
    DoOutputDebugString("BreakpointOnReturn: breakpoint set with register %d.", Register);
    return TRUE;
}

BOOL SetInitialBreakpoints(PVOID ImageBase)
{
    DWORD_PTR BreakpointVA;
    DWORD Register;

    if (BreakpointsHit)
        return TRUE;

    StepCount = 0;
    TraceDepthCount = 0;
    StopTrace = FALSE;

    TraceAll = g_config.trace_all;

	if (!bp0 && !bp1 && !bp2 && !bp3 && !bpw0 && !bpw1 && !bpw2 && !bpw3 && !EntryPointRegister)
		EntryPointRegister = 1;

	if (g_config.break_on_return_set)
		EntryPointRegister = 0;

	if (g_config.break_on_apiname_set)
		EntryPointRegister = 0;

    if (EntryPointRegister && !StepLimit && TraceDepthLimit == 0xFFFFFFFF && !TraceAll)
    {
        DebuggerOutput("Options:\n");

        if (EntryPointRegister)
            DebuggerOutput("    - bp0=<RVA or VA>, break-on-return=<API> to set initial breakpoints, defaulting to bp0 on entry point.\n");

        if (!StepLimit)
        {
            StepLimit = SINGLE_STEP_LIMIT;
            DebuggerOutput("    - count=<count> for number of instructions or steps to trace - defaulting to 0x%x.\n", StepLimit);
        }

        if (TraceDepthLimit == 0xFFFFFFFF)
        {
            TraceDepthLimit = 1;
            DebuggerOutput("    - depth=<depth> to step into or over calls - defaulting to a depth of %d.\n", TraceDepthLimit);
        }

        if (!ImageBase)
        {
            ImageBase = GetModuleHandle(NULL);
            DebuggerOutput("ImageBase not set by base-on-api parameter, defaulting to process image base 0x%p.\n", ImageBase);
        }
        else
            DebuggerOutput("ImageBase set to 0x%p.\n", ImageBase);

        if (TraceAll)
            DebuggerOutput("Trace mode: All.\n");
    }
    else
    {
        if (!StepLimit)
            StepLimit = SINGLE_STEP_LIMIT;

        if (TraceDepthLimit == 0xFFFFFFFF)
            TraceDepthLimit = 1;

        if (!ImageBase)
            ImageBase = GetModuleHandle(NULL);
    }

#ifdef STANDALONE
    TraceDepthLimit = 5;
#endif

    if (g_config.break_on_apiname_set)
    {
        HANDLE Module = GetModuleHandle(g_config.break_on_modname);
        if (Module)
            bp0 = GetProcAddress(Module, g_config.break_on_apiname);
        else
            DebuggerOutput("Failed to get base for module (%s).", g_config.break_on_modname);
        if (bp0)
            DebuggerOutput("bp0 set to 0x%p (%s::%s).", bp0, g_config.break_on_modname, g_config.break_on_apiname);
        else
            DebuggerOutput("Failed to get address for function %s::%s.", g_config.break_on_modname, g_config.break_on_apiname);
    }

    if (EntryPointRegister)
    {
        PVOID EntryPoint = (PVOID)GetEntryPointVA((DWORD_PTR)ImageBase);

        if (EntryPoint)
        {
            Register = EntryPointRegister - 1;

            if (SetBreakpoint(Register, 0, (BYTE*)EntryPoint, BP_EXEC, BreakpointCallback))
            {
                DebuggerOutput("Breakpoint %d set on entry point at 0x%p.", Register, EntryPoint);
                BreakpointsSet = TRUE;
                bp0 = EntryPoint;
            }
            else
            {
                DebuggerOutput("SetBreakpoint on entry point failed.\n");
                BreakpointsSet = FALSE;
                return FALSE;
            }
        }
    }
    else if (bp0)
    {
        Register = 0;
        PVOID Callback;

        if (g_config.file_offsets)
        {
            if (!IsDisguisedPEHeader(ImageBase))
            {
                DoOutputDebugString("SetInitialBreakpoints: File offsets cannot be applied to non-PE image at 0x%p.\n", ImageBase);
                BreakpointsSet = FALSE;
                return FALSE;
            }
            BreakpointVA = FileOffsetToVA((DWORD_PTR)ImageBase, (DWORD_PTR)bp0);
        }
        else
        {
            if ((SIZE_T)bp0 > RVA_LIMIT)
                BreakpointVA = (DWORD_PTR)bp0;
            else
                BreakpointVA = (DWORD_PTR)ImageBase + (DWORD_PTR)bp0;
        }

        if (!Type0)
        {
            Type0 = BP_EXEC;
            Callback = BreakpointCallback;
        }
        else if (Type0 == BP_WRITE)
            Callback = BreakpointCallback;
            //Callback = WriteCallback;

        if (SetBreakpoint(Register, 0, (BYTE*)BreakpointVA, Type0, Callback))
        {
            DebuggerOutput("Breakpoint %d set on address 0x%p (RVA 0x%x, type %d)\n", Register, BreakpointVA, bp0, Type0);
            BreakpointsSet = TRUE;
        }
        else
        {
            DebuggerOutput("SetBreakpoint failed for breakpoint %d.\n", Register);
            BreakpointsSet = FALSE;
            return FALSE;
        }
    }

    if (bp1)
    {
        Register = 1;
        PVOID Callback;

        if (g_config.file_offsets)
            BreakpointVA = FileOffsetToVA((DWORD_PTR)ImageBase, (DWORD_PTR)bp1);
        else
        {
            if ((SIZE_T)bp1 > RVA_LIMIT)
                BreakpointVA = (DWORD_PTR)bp1;
            else
                BreakpointVA = (DWORD_PTR)ImageBase + (DWORD_PTR)bp1;
        }

        if (!Type1)
        {
            Type1 = BP_EXEC;
            Callback = BreakpointCallback;
        }
        else if (Type1 == BP_WRITE)
            Callback = WriteCallback;

        if (SetBreakpoint(Register, 0, (BYTE*)BreakpointVA, Type1, Callback))
        {
            DebuggerOutput("Breakpoint %d set on address 0x%p (RVA 0x%x, type %d)\n", Register, BreakpointVA, bp1, Type1);
            BreakpointsSet = TRUE;
        }
        else
        {
            DebuggerOutput("SetBreakpoint failed for breakpoint %d.\n", Register);
            BreakpointsSet = FALSE;
            return FALSE;
        }
    }

    if (bp2)
    {
        Register = 2;
        PVOID Callback;

        if (g_config.file_offsets)
            BreakpointVA = FileOffsetToVA((DWORD_PTR)ImageBase, (DWORD_PTR)bp2);
        else
        {
            if ((SIZE_T)bp2 > RVA_LIMIT)
                BreakpointVA = (DWORD_PTR)bp2;
            else
                BreakpointVA = (DWORD_PTR)ImageBase + (DWORD_PTR)bp2;
        }

        if (!Type2)
        {
            Type1 = BP_EXEC;
            Callback = BreakpointCallback;
        }
        else if (Type2 == BP_WRITE)
            Callback = WriteCallback;

        if (SetBreakpoint(Register, 0, (BYTE*)BreakpointVA, Type2, Callback))
        {
            DebuggerOutput("Breakpoint %d set on address 0x%p (RVA 0x%x, type %d)\n", Register, BreakpointVA, bp2, Type2);
            BreakpointsSet = TRUE;
        }
        else
        {
            DebuggerOutput("SetBreakpoint failed for breakpoint %d.\n", Register);
            BreakpointsSet = FALSE;
            return FALSE;
        }
    }

    if (bp3)
    {
        Register = 3;
        PVOID Callback;

        if (g_config.file_offsets)
            BreakpointVA = FileOffsetToVA((DWORD_PTR)ImageBase, (DWORD_PTR)bp3);
        else
        {
            if ((SIZE_T)bp3 > RVA_LIMIT)
                BreakpointVA = (DWORD_PTR)bp3;
            else
                BreakpointVA = (DWORD_PTR)ImageBase + (DWORD_PTR)bp3;
        }

        if (!Type3)
        {
            Type1 = BP_EXEC;
            Callback = BreakpointCallback;
        }
        else if (Type3 == BP_WRITE)
            Callback = WriteCallback;

        if (SetBreakpoint(Register, 0, (BYTE*)BreakpointVA, Type3, Callback))
        {
            DebuggerOutput("Breakpoint %d set on address 0x%p (RVA 0x%x, type %d)\n", Register, BreakpointVA, bp3, Type3);
            BreakpointsSet = TRUE;
        }
        else
        {
            DebuggerOutput("SetBreakpoint failed for breakpoint %d.\n", Register);
            BreakpointsSet = FALSE;
            return FALSE;
        }
    }

    return BreakpointsSet;
}