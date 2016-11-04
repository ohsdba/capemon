/*
CAPE - Config And Payload Extraction
Copyright(C) 2015, 2016 Context Information Security. (kevin.oreilly@contextis.com)

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
#include <tchar.h>
#include <windows.h>
#include <Wincrypt.h>
#include <WinNT.h>
#include <Shlwapi.h>
#include <stdint.h>
#include <Psapi.h>

#include "CAPE.h"
#include "Debugger.h"
#include "..\pipe.h"
#include "..\config.h"

#pragma comment(lib, "Shlwapi.lib")

#define BUFSIZE 			1024	// For hashing
#define MD5LEN  			16
#define DUMP_MAX            100     
#define CAPE_OUTPUT_FILE "CapeOutput.bin"

static unsigned int DumpCount;
 
extern uint32_t path_from_handle(HANDLE handle, wchar_t *path, uint32_t path_buffer_len);

#define CAPE_OUTPUT_FILE "CapeOutput.bin"

extern void DoOutputDebugString(_In_ LPCTSTR lpOutputString, ...);
extern void DoOutputErrorString(_In_ LPCTSTR lpOutputString, ...);
extern void CapeOutputFile(LPCTSTR lpOutputFile);
extern int ScyllaDumpCurrentProcess(DWORD NewOEP);
extern int ScyllaDumpProcess(HANDLE hProcess, DWORD_PTR modBase, DWORD NewOEP);
extern int ScyllaDumpCurrentProcessFixImports(DWORD NewOEP);
extern int ScyllaDumpProcessFixImports(HANDLE hProcess, DWORD_PTR modBase, DWORD NewOEP);

extern wchar_t *our_process_path;
extern ULONG_PTR base_of_dll_of_interest;

static HMODULE s_hInst = NULL;
static WCHAR s_wzDllPath[MAX_PATH];
CHAR s_szDllPath[MAX_PATH];

BOOL ProcessDumped;

//**************************************************************************************
void PrintHexBytes(__in char* TextBuffer, __in BYTE* HexBuffer, __in unsigned int Count)
//**************************************************************************************
{
	unsigned int i;
	
	if (HexBuffer == NULL)
		return;
	
	for (i=0; i<Count; i++)
	{
		sprintf_s((TextBuffer+2*i), Count, "%2.2x", (unsigned int)*(HexBuffer+i));	
	}
	
	return;
}

//**************************************************************************************
BOOL SetCapeMetaData(DWORD DumpType, DWORD TargetPid, HANDLE hTargetProcess, PVOID Address)
//**************************************************************************************
{
    if (DumpType == 0)
    {
        DoOutputDebugString("SetCapeMetaData: DumpType NULL.\n");
        return FALSE;
    }

    CapeMetaData->DumpType = DumpType;

    if (DumpType == INJECTION_PE || DumpType == INJECTION_SHELLCODE)
    {
        if (!TargetPid)
        {
            DoOutputDebugString("SetCapeMetaData: Injection type with no PID - error.\n");
            return FALSE;
        }
        
        if (!hTargetProcess)
        {
            DoOutputDebugString("SetCapeMetaData: Injection type with no process handle - error.\n");
            return FALSE;
        }
        
        CapeMetaData->TargetPid = TargetPid;
        
        if (CapeMetaData->TargetProcess == NULL)
        {
            DoOutputDebugString("SetCapeMetaData: failed to allocate memory for target process string.\n");
            return FALSE;
        }
        
        if (CapeMetaData->TargetProcess == NULL && !GetModuleFileNameEx(hTargetProcess, NULL, CapeMetaData->TargetProcess, MAX_PATH))
        {
            CapeMetaData->TargetProcess = (char*)malloc(MAX_PATH);
            DoOutputErrorString("SetCapeMetaData: GetModuleFileNameEx failed on target process, handle 0x%x", hTargetProcess);
            return FALSE;
        }
    }
    else if (DumpType == EXTRACTION_PE || DumpType == EXTRACTION_SHELLCODE)
    {
        if (!Address)
        {
            DoOutputDebugString("SetCapeMetaData: Extraction type with no PID - error.\n");
            return FALSE;
        }

        CapeMetaData->Address = Address;
    }

	return TRUE;
}

//**************************************************************************************
BOOL MapFile(HANDLE hFile, unsigned char **Buffer, DWORD* FileSize)
//**************************************************************************************
{
	LARGE_INTEGER LargeFileSize;
	DWORD dwBytesRead;
	
	if (!GetFileSizeEx(hFile, &LargeFileSize))
	{
		DoOutputErrorString("MapFile: Cannot get file size");
		return FALSE;
	}

    if (LargeFileSize.HighPart || LargeFileSize.LowPart > SIZE_OF_LARGEST_IMAGE)
	{
		DoOutputDebugString("MapFile: File too big");
		return FALSE;
	}

    if (LargeFileSize.LowPart == 0)
	{
		DoOutputDebugString("MapFile: File is zero in size.");
		return FALSE;
	}

	*FileSize = LargeFileSize.LowPart;
	
    DoOutputDebugString("File size: 0x%x", *FileSize);
	
	*Buffer = malloc(*FileSize);
	
    if (SetFilePointer(hFile, 0, 0, FILE_BEGIN))
    {
 		DoOutputErrorString("MapFile: Failed to set file pointer");
		return FALSE;   
    }
    
	if (*Buffer == NULL)
	{
		DoOutputErrorString("MapFile: Memory allocation error in MapFile");
		return FALSE;
	}
	
	if (FALSE == ReadFile(hFile, (LPVOID)*Buffer, *FileSize, &dwBytesRead, NULL))
	{
		DoOutputErrorString("ReadFile error");
        free(Buffer);
		return FALSE;
	}

    if (dwBytesRead > 0 && dwBytesRead < *FileSize)
    {
        DoOutputErrorString("MapFile: Unexpected size read in");
        free(Buffer);
		return FALSE;

    }
    else if (dwBytesRead == 0)
    {
        DoOutputErrorString("MapFile: No data read from file");
        free(Buffer);
		return FALSE;
    }
	
	return TRUE;
}

//**************************************************************************************
PINJECTIONINFO GetInjectionInfo(DWORD ProcessId)
//**************************************************************************************
{
    DWORD CurrentProcessId;  
	
    PINJECTIONINFO CurrentInjectionInfo = InjectionInfoList;

	while (CurrentInjectionInfo)
	{
		CurrentProcessId = CurrentInjectionInfo->ProcessId;
        
        if (CurrentProcessId == ProcessId)
            return CurrentInjectionInfo;
		else
            CurrentInjectionInfo = CurrentInjectionInfo->NextInjectionInfo;
	}
    
	return NULL;
}

//**************************************************************************************
PINJECTIONINFO CreateInjectionInfo(DWORD ProcessId)
//**************************************************************************************
{
	PINJECTIONINFO CurrentInjectionInfo, PreviousInjectionInfo;

    PreviousInjectionInfo = NULL;
    
	if (InjectionInfoList == NULL)
	{
		InjectionInfoList = ((struct InjectionInfo*)malloc(sizeof(struct InjectionInfo)));
		
        if (InjectionInfoList == NULL)
        {
            DoOutputDebugString("CreateInjectionInfo: failed to allocate memory for initial injection info list.\n");
            return NULL;
        }
        
        memset(InjectionInfoList, 0, sizeof(struct InjectionInfo));
		
        InjectionInfoList->ProcessId = ProcessId;
	}

	CurrentInjectionInfo = InjectionInfoList;
    
    while (CurrentInjectionInfo)
	{  
        //if (CurrentInjectionInfo->ProcessId && CurrentInjectionInfo->ProcessId == ProcessId)
        //{
        //    //It already exists - shouldn't happen
        //    DoOutputDebugString("CreateInjectionInfo error: found an existing injection info struct for ProcessId 0x%x\n", ProcessId);
        //    return NULL;
        //}
        
        if ((CurrentInjectionInfo->ProcessId) == ProcessId)
        {
            // We have our injection info struct
            break;            
        }
        
		PreviousInjectionInfo = CurrentInjectionInfo;
        CurrentInjectionInfo = CurrentInjectionInfo->NextInjectionInfo;
	}
	
    if (!CurrentInjectionInfo)
    {
        // We haven't found it in the linked list, so create a new one
        CurrentInjectionInfo = PreviousInjectionInfo;
        
        CurrentInjectionInfo->NextInjectionInfo = ((struct InjectionInfo*)malloc(sizeof(struct InjectionInfo)));
	
        if (CurrentInjectionInfo->NextInjectionInfo == NULL)
		{
			DoOutputDebugString("CreateInjectionInfo: Failed to allocate new thread breakpoints.\n");
			return NULL;
		}
        
        memset(CurrentInjectionInfo->NextInjectionInfo, 0, sizeof(struct InjectionInfo));
        
        CurrentInjectionInfo = CurrentInjectionInfo->NextInjectionInfo;
	}
    
    return CurrentInjectionInfo;
}

//**************************************************************************************
char* GetName()
//**************************************************************************************
{
    char *OutputFilename;
    SYSTEMTIME Time;

    OutputFilename = (char*)malloc(MAX_PATH);
    
    if (OutputFilename == NULL)
    {
        DoOutputErrorString("GetName: failed to allocate memory");
        return 0;
    }
    
    GetSystemTime(&Time);
    sprintf_s(OutputFilename, MAX_PATH*sizeof(char), "%d_%d%d%d%d%d%d%d%d", GetCurrentProcessId(), Time.wMilliseconds, Time.wSecond, Time.wMinute, Time.wHour, Time.wDay, Time.wDayOfWeek, Time.wMonth, Time.wYear);
    
	return OutputFilename;
}

//**************************************************************************************
BOOL GetHash(unsigned char* Buffer, unsigned int Size, char* OutputFilenameBuffer)
//**************************************************************************************
{
	DWORD i;
	DWORD dwStatus = 0;
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    DWORD cbHash = 0;
    BYTE MD5Hash[MD5LEN];

    // Get handle to the crypto provider
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        DoOutputErrorString("CryptAcquireContext failed");
        return 0;
    }

    if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash))
    {
        DoOutputErrorString("CryptCreateHash failed"); 
        CryptReleaseContext(hProv, 0);
        return 0;
    }

	if (!CryptHashData(hHash, Buffer, Size, 0))
	{
		DoOutputErrorString("CryptHashData failed"); 
		CryptReleaseContext(hProv, 0);
		CryptDestroyHash(hHash);
		return 0;
	}

    cbHash = MD5LEN;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, MD5Hash, &cbHash, 0))
    {
        DoOutputErrorString("CryptGetHashParam failed"); 
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
	
	for (i = 0; i < cbHash; i++)
	{
		PrintHexBytes(OutputFilenameBuffer, MD5Hash, MD5LEN);
	}
	
	return 1;
}

//**************************************************************************************
char* GetHashFromHandle(HANDLE hFile)
//**************************************************************************************
{
    DWORD FileSize;
	long e_lfanew;
	PIMAGE_NT_HEADERS pNtHeader;
	unsigned char* Buffer = NULL;
	char * OutputFilenameBuffer;

	if (!MapFile(hFile, &Buffer, &FileSize))
	{	
		DoOutputErrorString("MapFile error - check the path is valid and the file has size.");
		return 0;
	}
    
	OutputFilenameBuffer = (char*) malloc(MAX_PATH);

    if (OutputFilenameBuffer == NULL)
    {
		DoOutputErrorString("Error allocating memory for hash string");
		return 0;    
    }
    
    if (!GetHash(Buffer, FileSize, (char*)OutputFilenameBuffer))
    {
		DoOutputErrorString("GetHashFromHandle: GetHash function failed");
		return 0;    
    }
    
    DoOutputDebugString("GetHash returned: %s", OutputFilenameBuffer);

    // Check if we have a valid DOS and PE header at the beginning of Buffer
    if (*(WORD*)Buffer == IMAGE_DOS_SIGNATURE)
    {
        e_lfanew = *(long*)(Buffer+0x3c);

        if ((unsigned int)e_lfanew>PE_HEADER_LIMIT)
        {
            // This check is possibly not appropriate here
            // As long as we've got what's been compressed
        }

        if (*(DWORD*)(Buffer+e_lfanew) == IMAGE_NT_SIGNATURE)
        {
            pNtHeader = (PIMAGE_NT_HEADERS)(Buffer+e_lfanew);

            if ((pNtHeader->FileHeader.Characteristics & IMAGE_FILE_DLL) == IMAGE_FILE_DLL)
            {
                sprintf_s((OutputFilenameBuffer+2*MD5LEN), MAX_PATH*sizeof(char), ".dll");
            }
            else if ((pNtHeader->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == IMAGE_FILE_EXECUTABLE_IMAGE)
            {
                sprintf_s((OutputFilenameBuffer+2*MD5LEN), MAX_PATH*sizeof(char)-2*MD5LEN, ".exe_");
            }
        }
    }
    
    CloseHandle(hFile);
    
	// We don't need the file buffer any more
    free(Buffer);
    
    // We leak the OutputFilenameBuffer
    return OutputFilenameBuffer;
}

//**************************************************************************************
int DumpXorPE(LPBYTE Buffer, unsigned int Size)
//**************************************************************************************
{
	LONG e_lfanew;
    DWORD NT_Signature, FullKey;
	WORD TestKey;
    unsigned int i, j, k, rotation;
	BYTE* DecryptedBuffer;

    for (i=0; i<=0xFF; i++)
	{
		// check for the DOS signature a.k.a MZ header
		if ((*Buffer^(BYTE)i) == 'M' && (*(Buffer+1)^(BYTE)i) == 'Z')
		{
			DoOutputDebugString("MZ header found with bytewise XOR key 0x%.2x\n", i);

			e_lfanew = (LONG)*(DWORD*)(Buffer+0x3c);

            DoOutputDebugString("Encrypted e_lfanew: 0x%x", e_lfanew);
            
			for (j=0; j<sizeof(LONG); j++)
				*((BYTE*)&e_lfanew+j) = *((BYTE*)&e_lfanew+j)^i;

            DoOutputDebugString("Decrypted e_lfanew: 0x%x", e_lfanew);
            
			if ((unsigned int)e_lfanew > PE_HEADER_LIMIT)
			{	
				DoOutputDebugString("The pointer to the PE header seems a tad large: 0x%x", e_lfanew);
				//return FALSE;
			}

			// let's get the NT signature a.k.a PE header
			memcpy(&NT_Signature, Buffer+e_lfanew, 4);
            
            DoOutputDebugString("Encrypted NT_Signature: 0x%x", NT_Signature);
			
			// let's try decrypting it with the key
			for (k=0; k<4; k++)
				*((BYTE*)&NT_Signature+k) = *((BYTE*)&NT_Signature+k)^i;

            DoOutputDebugString("Encrypted NT_Signature: 0x%x", NT_Signature);

			// does it check out?
			if (NT_Signature == IMAGE_NT_SIGNATURE)
			{
				DoOutputDebugString("Xor-encrypted PE detected, about to dump.\n");
                
                DecryptedBuffer = (BYTE*)malloc(Size);
                
                if (DecryptedBuffer == NULL)
                {
                    DoOutputErrorString("Error allocating memory for decrypted PE binary");
                    return FALSE;
                }
                
                memcpy(DecryptedBuffer, Buffer, Size);
                
                for (k=0; k<Size; k++)
                    *(DecryptedBuffer+k) = *(DecryptedBuffer+k)^i;
                
                DumpPE(DecryptedBuffer);
                
                free(DecryptedBuffer);
				return TRUE;
			}
			else
			{
				DoOutputDebugString("PE signature invalid, looks like a false positive! 1 in 0x10000!!\n");
				return FALSE;
			}
		}
	}
	
#ifndef _WIN64
	for (i=0; i<=0xffff; i++)
	{
		// check for the DOS signature a.k.a MZ header
		if ((*(WORD*)Buffer^(WORD)i) == IMAGE_DOS_SIGNATURE)
		{
			DoOutputDebugString("MZ header found with wordwise XOR key 0x%.2x%.2x\n", *(BYTE*)&i, *((BYTE*)&i+1));
			
			// let's try just the little end of the full lfanew which is almost always the whole value anyway
			e_lfanew = *(WORD*)(Buffer+0x3c);

			// try and decrypt
			e_lfanew = e_lfanew^(WORD)i;

			if ((unsigned int)e_lfanew > PE_HEADER_LIMIT)
			{	
				// even if dword-encrypted, 
				// if the little endian word of the dword takes it too far it's over
				DoOutputDebugString("Sadly the pointer to the PE header seems a tad too large: 0x%x", e_lfanew);
				//return FALSE;
			}

			// get PE header
			memcpy(&NT_Signature, Buffer+e_lfanew, 4);
			
			// We need to rotate our key for a non-dword aligned offset
			TestKey = i;
			if (e_lfanew % 2)
			{
				__asm 
				{	
					mov ax, TestKey
					ror ax, 8
					mov TestKey, ax
				}
			}				

			// let's try decrypting it with the word key
			for (k=0; k<2; k++)
				*((WORD*)&NT_Signature+k) = *((WORD*)&NT_Signature+k)^TestKey;
				
			// does it check out?
			if (NT_Signature == IMAGE_NT_SIGNATURE)
			{
				DoOutputDebugString("Xor-encrypted PE detected, about to dump.\n");
                
                DecryptedBuffer = (BYTE*)malloc(Size);
                
                if (DecryptedBuffer == NULL)
                {
                    DoOutputErrorString("Error allocating memory for decrypted PE binary");
                    return FALSE;
                }
                
                memcpy(DecryptedBuffer, Buffer, Size);
                
                for (k=0; k<Size; k=k+2)
                    *(WORD*)(DecryptedBuffer+k) = *(WORD*)(DecryptedBuffer+k)^TestKey;
                
                DumpPE(DecryptedBuffer);
                
                free(DecryptedBuffer);
				return TRUE;
			}
			else if ((WORD)NT_Signature == (WORD)IMAGE_NT_SIGNATURE)
			{
				// looks like DWORD encrypted with zero most significant word of lfanew
				// let's confirm
				DWORD FullKey = TestKey + ((*(WORD*)(Buffer+0x3e))<<16);

				// let's recopy our candidate PE header
				memcpy(&NT_Signature, Buffer+e_lfanew, 4);

				// We need to rotate our key for a non-dword aligned offset
				for (rotation = 0; rotation<(unsigned int)(e_lfanew % 4); rotation++)
				{
					__asm 
					{	
						mov eax, FullKey
						ror eax, 8
						mov FullKey, eax
					}
				}	
			
				// final test of the latter two bytes of PE header
				// (might as well test the whole thing)
				if ((NT_Signature ^ FullKey) == IMAGE_NT_SIGNATURE)
                {
                    DoOutputDebugString("Xor-encrypted PE detected, about to dump.\n");
                    
                    DecryptedBuffer = (BYTE*)malloc(Size);
                    
                    if (DecryptedBuffer == NULL)
                    {
                        DoOutputErrorString("Error allocating memory for decrypted PE binary");
                        return FALSE;
                    }
                    
                    memcpy(DecryptedBuffer, Buffer, Size);
                    
                    for (k=0; k<Size; k=k+4)
                        *(DWORD*)(DecryptedBuffer+k) = *(DWORD*)(DecryptedBuffer+k)^FullKey;
                    
                    DumpPE(DecryptedBuffer);
                    
                    free(DecryptedBuffer);
                    return TRUE;
                }
                else
				{
					// There's *very* remote this was a false positive, we should continue
					continue;
				}
			}

			// could be dword with non-zero most signicant bytes of lfanew
			// brute force the 0xffff possibilities here
			
			for (TestKey=0; TestKey<0xffff; TestKey++)
			{
				long full_lfanew = e_lfanew + (0x10000*((*(WORD*)(Buffer+0x3e))^TestKey));						
				
				if ((unsigned int)full_lfanew > PE_HEADER_LIMIT)
				{	
					continue;			
				}

				memcpy(&NT_Signature, Buffer+full_lfanew, 4);

				// We need to rotate our key for a non-dword aligned offset
				FullKey = i + (TestKey<<16);
				for (rotation = 0; rotation<(unsigned int)(full_lfanew % 4); rotation++)
				{
					__asm 
					{	
						mov eax, FullKey
						ror eax, 8
						mov FullKey, eax
					}
				}

				// let's try decrypting it with the key
				if ((NT_Signature ^ FullKey) == IMAGE_NT_SIGNATURE)
                {
                    DoOutputDebugString("Xor-encrypted PE detected, about to dump.\n");
                    
                    DecryptedBuffer = (BYTE*)malloc(Size);
                    
                    if (DecryptedBuffer == NULL)
                    {
                        DoOutputErrorString("Error allocating memory for decrypted PE binary");
                        return FALSE;
                    }
                    
                    memcpy(DecryptedBuffer, Buffer, Size);
                    
                    for (k=0; k<Size; k=k+4)
                        *(DWORD*)(DecryptedBuffer+k) = *(DWORD*)(DecryptedBuffer+k)^FullKey;
                    
                    DumpPE(DecryptedBuffer);
                    
                    free(DecryptedBuffer);
                    return TRUE;
                }
			}
		}
	}

#endif
    // We free can free DecryptedBuffer as it's no longer needed
    free(DecryptedBuffer);
    
    return FALSE;
}

//**************************************************************************************
int ScanForNonZero(LPCVOID Buffer, unsigned int Size)
//**************************************************************************************
{
    unsigned int p;
    
    for (p=0; p<Size-1; p++)
        if (*((char*)Buffer+p) != 0)
            return 1;

    return 0;
}

//**************************************************************************************
int ScanForPE(LPCVOID Buffer, unsigned int Size, LPCVOID* Offset)
//**************************************************************************************
{
    unsigned int p;
    PIMAGE_DOS_HEADER pDosHeader;
    PIMAGE_NT_HEADERS pNtHeader;
    
    if (Size == 0)
    {
        DoOutputDebugString("ScanForPE: Error, zero size given\n");
        return 0;
    }
    
    for (p=0; p<Size-1; p++)
    {
        if (*((char*)Buffer+p) == 'M' && *((char*)Buffer+p+1) == 'Z')
        {
            pDosHeader = (PIMAGE_DOS_HEADER)((char*)Buffer+p);

            if ((ULONG)pDosHeader->e_lfanew == 0) 
            {
                // e_lfanew is zero
                continue;
            }

            if ((ULONG)pDosHeader->e_lfanew > Size-p)
            {
                // e_lfanew points beyond end of region
                continue;
            }
            
            pNtHeader = (PIMAGE_NT_HEADERS)((PCHAR)pDosHeader + (ULONG)pDosHeader->e_lfanew);
            
            if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) 
            {
                // No 'PE' header
                continue;                
            }
            
            if ((pNtHeader->FileHeader.Machine == 0) || (pNtHeader->FileHeader.SizeOfOptionalHeader == 0)) 
            {
                // Basic requirements
                DoOutputDebugString("ScanForPE: Basic requirements failure.\n");
                continue;
            }
            
            if (Offset)
            {
                *Offset = (LPCVOID)((char*)Buffer+p);
            }
            
            DoOutputDebugString("ScanForPE: success!\n");
            
            return 1;
        }
    }
    
    return 0;
}

//**************************************************************************************
int DumpMemory(LPCVOID Buffer, unsigned int Size)
//**************************************************************************************
{
	char *OutputFilename, *FullPathName;
	DWORD RetVal, dwBytesWritten;
	HANDLE hOutputFile;
    LPVOID BufferCopy;

	FullPathName = (char*) malloc(MAX_PATH);

    if (FullPathName == NULL)
    {
		DoOutputErrorString("DumpMemory: Error allocating memory for strings");
		return 0;    
    }
    
    OutputFilename = GetName();
    
	// We want to dump CAPE output to the 'analyzer' directory
    memset(FullPathName, 0, MAX_PATH);
	
    strncpy_s(FullPathName, MAX_PATH, g_config.analyzer, strlen(g_config.analyzer)+1);

	if (strlen(FullPathName) + strlen("\\CAPE\\") + strlen(OutputFilename) >= MAX_PATH)
	{
		DoOutputDebugString("DumpMemory: Error, CAPE destination path too long.");
        free(OutputFilename); free(FullPathName);
		return 0;
	}

    PathAppend(FullPathName, "CAPE");

	RetVal = CreateDirectory(FullPathName, NULL);

	if (RetVal == 0 && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		DoOutputDebugString("DumpMemory: Error creating output directory");
        free(OutputFilename); free(FullPathName);
		return 0;
	}

    PathAppend(FullPathName, OutputFilename);
	
    DoOutputDebugString("DumpMemory: FullPathName = %s", FullPathName);
    
	hOutputFile = CreateFile(FullPathName, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    
	if (hOutputFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS)
	{
		DoOutputDebugString("DumpMemory: CAPE output filename exists already: %s", FullPathName);
        free(OutputFilename); free(FullPathName);
		return 0;
	}

	if (hOutputFile == INVALID_HANDLE_VALUE)
	{
		DoOutputErrorString("DumpMemory: Could not create CAPE output file");
        free(OutputFilename); free(FullPathName);
		return 0;		
	}	
	
	dwBytesWritten = 0;
    
    DoOutputDebugString("DumpMemory: CAPE output file succssfully created: %s", FullPathName);

	BufferCopy = (LPVOID)((BYTE*)malloc(Size));
    
    if (BufferCopy == NULL)
    {
        DoOutputDebugString("DumpMemory: Failed to allocate memory for buffer copy.\n");
        return FALSE;
    }
    
    memcpy(BufferCopy, Buffer, Size);
    
    if (FALSE == WriteFile(hOutputFile, BufferCopy, Size, &dwBytesWritten, NULL))
	{
		DoOutputErrorString("DumpMemory: WriteFile error on CAPE output file");
        free(OutputFilename); free(FullPathName); free(BufferCopy);
		return 0;
	}

	CloseHandle(hOutputFile);
    
    CapeOutputFile(FullPathName);
    
    // We can free the filename buffers
    free(OutputFilename); free(FullPathName); free(BufferCopy);
	
    return 1;
}

//**************************************************************************************
int DumpCurrentProcessFixImports(DWORD NewEP)
//**************************************************************************************
{
	if (DumpCount < DUMP_MAX && ScyllaDumpCurrentProcessFixImports(NewEP))
	{
		DumpCount++;
		return 1;
	}

	return 0;
}

//**************************************************************************************
int DumpCurrentProcessNewEP(DWORD NewEP)
//**************************************************************************************
{
	if (DumpCount < DUMP_MAX && ScyllaDumpCurrentProcess(NewEP))
	{
		DumpCount++;
		return 1;
	}

	return 0;
}

//**************************************************************************************
int DumpCurrentProcess()
//**************************************************************************************
{
	if (DumpCount < DUMP_MAX && ScyllaDumpCurrentProcess(0))
	{
		DumpCount++;
		return 1;
	}

	return 0;
}

//**************************************************************************************
int DumpModuleInCurrentProcess(DWORD ModuleBase)
//**************************************************************************************
{
	if (DumpCount < DUMP_MAX && ScyllaDumpProcess(GetCurrentProcess(), ModuleBase, 0))
	{
        DumpCount++;
		return 1;
	}

	return 0;
}
//**************************************************************************************
int DumpImageInCurrentProcess(DWORD ImageBase)
//**************************************************************************************
{
    PIMAGE_DOS_HEADER pDosHeader;
    PIMAGE_NT_HEADERS pNtHeader;
    
    pDosHeader = (PIMAGE_DOS_HEADER)ImageBase;

	if (DumpCount >= DUMP_MAX)
	{
        DoOutputDebugString("DumpImageInCurrentProcess: CAPE dump limit reached.\n");
        return 0;
    }

    if (*(WORD*)ImageBase != IMAGE_DOS_SIGNATURE)
    {
        DoOutputDebugString("DumpImageInCurrentProcess: No DOS signature in header.\n");
        return 0;
    }
    
    if (!pDosHeader->e_lfanew || pDosHeader->e_lfanew > PE_HEADER_LIMIT)
    {
        DoOutputDebugString("DumpImageInCurrentProcess: bad e_lfanew.\n");
        return 0;    
    }
 
    pNtHeader = (PIMAGE_NT_HEADERS)((PCHAR)pDosHeader + (ULONG)pDosHeader->e_lfanew);
    
    if (pNtHeader->Signature != IMAGE_NT_SIGNATURE) 
    {
        // No 'PE' header
        DoOutputDebugString("DumpImageInCurrentProcess: Invalid PE signature in header.\n");
        return 0;
    }
    
    if ((pNtHeader->FileHeader.Machine == 0) || (pNtHeader->FileHeader.SizeOfOptionalHeader == 0 || pNtHeader->OptionalHeader.SizeOfHeaders == 0)) 
    {
        // Basic requirements
        DoOutputDebugString("DumpImageInCurrentProcess: PE image invalid.\n");
        return 0;
    }
        
    // we perform a couple of tests to determine whether this is a 'raw' or 'virtual' image
    // first we check if the SizeOfHeaders is a multiple of FileAlignment
    if (pNtHeader->OptionalHeader.SizeOfHeaders % pNtHeader->OptionalHeader.FileAlignment
    // let's also check if section 1 actually begins after the headers, i.e. raw image
        || (*((BYTE*)ImageBase + pNtHeader->OptionalHeader.SizeOfHeaders - 1) == 0  // end of header is zero
        && *((BYTE*)ImageBase + pNtHeader->OptionalHeader.SizeOfHeaders) != 0))     // beginning of raw section 1 is non-zero
    {
        // looks like a 'raw'/'file' image, i.e. not loaded
        if (ScyllaDumpPE(ImageBase))
        {
            DoOutputDebugString("DumpImageInCurrentProcess: Attempting to dump 'raw' PE image.\n");
            DumpCount++;
            return 1; 
        }
        else
        {
            // failed to dump pe image
            DoOutputDebugString("DumpImageInCurrentProcess: Failed to dump 'raw' PE image.\n");
            return 0; 
        }
    }

    // we should be dealing with a 'loaded' module image now, 
    // not a 'raw' file image, so try dumping this way first
    DoOutputDebugString("DumpImageInCurrentProcess: Attempting to dump virtual PE image.\n");
    
    if (!ScyllaDumpProcess(GetCurrentProcess(), ImageBase, 0))
    // if this fails, let's try dumping 'raw' just in case
        if (!ScyllaDumpPE(ImageBase))
            return 0;

    DumpCount++;
    return 1;	
}

//**************************************************************************************
int DumpProcess(HANDLE hProcess, DWORD_PTR ImageBase)
//**************************************************************************************
{
	if (DumpCount < DUMP_MAX && ScyllaDumpProcess(hProcess, ImageBase, 0))
	{
		DumpCount++;
		return 1;
	}

	return 0;
}

//**************************************************************************************
int DumpPE(LPCVOID Buffer)
//**************************************************************************************
{
    if (DumpCount < DUMP_MAX && ScyllaDumpPE((DWORD_PTR)Buffer))
	{
        DumpCount++;
        return 1;
	}

	return 0;
}

//**************************************************************************************
int RoutineProcessDump()
//**************************************************************************************
{
    if (g_config.procmemdump && ProcessDumped == FALSE)
    {
        ProcessDumped = TRUE;   // this prevents a second call before the first is complete
        if (g_config.import_reconstruction)
        {   
            if (base_of_dll_of_interest)
                ProcessDumped = ScyllaDumpProcessFixImports(GetCurrentProcess(), base_of_dll_of_interest, 0);
            else
                ProcessDumped = ScyllaDumpCurrentProcessFixImports(0);
        }        
        else
        {
            if (base_of_dll_of_interest)
                ProcessDumped = ScyllaDumpProcess(GetCurrentProcess(), base_of_dll_of_interest, 0);
            else
                ProcessDumped = ScyllaDumpCurrentProcess(0);
        }
    }

	return ProcessDumped;
}

void init_CAPE()
{
    // Initialise CAPE global variables
    //
    CapeMetaData = (PCAPEMETADATA)malloc(sizeof(CAPEMETADATA));
    CapeMetaData->Pid = GetCurrentProcessId();    
    CapeMetaData->ProcessPath = (char*)malloc(MAX_PATH);
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, (LPCWSTR)our_process_path, wcslen(our_process_path)+1, CapeMetaData->ProcessPath, MAX_PATH, NULL, NULL);
 
#ifndef _WIN64	 
    // Start the debugger thread
    // if required by package
    if (DEBUGGER_ENABLED)
        launch_debugger();
#endif
    
    return;
}