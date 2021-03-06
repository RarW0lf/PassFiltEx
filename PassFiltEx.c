/*
PassFiltEx.c

Author: Joseph Ryan Ries 2018 <ryanries09@gmail.com> <ryan.ries@microsoft.com>

A password filter for Active Directory that uses a blacklist of bad passwords/character sequences.

Reference: https://msdn.microsoft.com/en-us/library/windows/desktop/ms721882(v=vs.85).aspx

********************************************************************************************
***                                     READ ME                                          ***
*** This is a personal project and is not in any way endorsed or supported by Microsoft. ***
*** Use at your own risk. This code is not guaranteed to be free of errors, and comes    ***
*** with no guarantees, liability, warranties or support.                                ***
***                                                                                      ***
********************************************************************************************

I wrote this just to join the club of people who can say that they've done it.

Installation:

  - Copy PassFiltEx.dll into the C:\Windows\System32 (or %SystemRoot%\System32) directory.

  - Copy the PassFiltExBlacklist.txt file into the C:\Windows\System32 (or %SystemRoot%\System32) directory.

  - (Or replace the text file with a list of your own. You are free to edit the blacklist file if you want.)

  - Edit the registry: HKLM\SYSTEM\CurrentControlSet\Control\Lsa => Notification Packages

  - Add PassFiltEx to the end of the list. (Do not include the file extension.) So the whole list of notification packages will read "rassfm scecli PassFiltEx" with newlines between each one.

  - Reboot the domain controller.

  - Repeat the above procedure on all domain controllers.

Operation:

  - Any time a user attempts to change his or her password, or any time an administrator attempts to set a user's password, the callback in this password filter will be invoked.

  - All password filters must say yes in order for the password change to be accepted. If any password filter says no, the password is not accepted. Therefore, this password filter does not
    need to check for password length, password complexity, password age, etc., because those things are already checked for using the in-box Windows password policy.

  - If a password contains any of the character sequences in the blacklist, *and* the blacklisted character sequence makes at least 50% of the password, then the password is rejected.

  - Comparisons are NOT case sensitive.

  - The blacklist is reloaded every 60 seconds, so feel free to edit the blacklist file at will. The password filter will read the new updates within a minute.

  - No Unicode support at this time. Everything is ASCII/ANSI. (You can still use Unicode characters in your passwords, but Unicode characters will not match against anything in the blacklist.)

  - Either Windows or Unix line endings (either \r\n or \n) should both work.
  
  - For example, if the blacklist contains the token "abc", then the passwords abc and abc123 and AbC123 and 123Abc will all be rejected. But Abc123! will be accepted, because the token abc 
    does not make up half (50%) of the full password or more.

  - Question: Why don't you store the blacklist file in SYSVOL? Answer: Might add that later. For now, I was concerned that having the blacklist file available for all Authenticated Users
    to read might pose a security threat, as it gives potential attackers a lot of information about which passwords you blacklist. For example, a hacker could feed your blacklist into his
	or her password cracker so that the password cracker would not attempt any blacklisted passwords, which would save the hacker time and give them fewer passwords to search for.
	So for now you'll need to copy the blacklist file to each DC and update it on each DC.

Debugging:

  - The password filter utilizes Event Tracing for Windows (ETW). ETW is fast, lightweight, and there is no concern over managing text-based log files which are slow and consume disk space.

  - The ETW provider for this password filter is 07d83223-7594-4852-babc-784803fdf6c5. So for example, you can enable tracing of the password filter on the next boot of the machine with:
    logman create trace autosession\PassFiltEx -o %SystemRoot%\Debug\PassFiltEx.etl -p "{07d83223-7594-4852-babc-784803fdf6c5}" 0xFFFFFFFF -ets

  - The trace will start when you reboot. To stop the trace, run: logman stop PassFiltEx -ets && logman delete autosession\PassFiltEx -ets

  - The StartTracingAtBoot.cmd and StopTracingAtBoot.cmd files provided contain these commands.

  - The other files, StartTracing.cmd and StopTracing.cmd will also enable the tracing, but the tracing will not persist across reboots.

  - Collect the *.etl file that is generated in the C:\Windows\debug directory. Then open the ETL file with a tool such as Microsoft Message Analyzer. 
    (There are other tools that understand ETW as well. Use what you like.) Add the "payload" as a Column, and decode the payload column as Unicode. 
	Then it should look like a normal, human-readable text log.

Coding Guidelines:

  - Want to contibute? Cool! I'd like to stick to these rules:

  - C only. (C++ is a Pandora's Box that I don't want to open.)

  - Compile with All Warnings (/Wall). Project should compile with 0 warnings. You MAY temporarily disable warnings with #pragmas if the warnings are too
    pedantic (e.g. don't warn me about adding padding bytes to structs or that a function was inlined.)

  - Use a static analyzer. The MSVC IDE comes with Code Analysis. Put it on "All Rules". You shouldn't trigger any Code Analysis warnings.

  - Define UNICODE. 
  
  - Prefix global symbols with a lower-case g, no underscore. (E.g. gGlobalVar, not g_GlobalVar)

  - Hungarian notation not necessary. Use descriptive variable names. We don't use 80-character terminals any more; it's OK to type it out.

  - Comment on why, not what.


*/




#define WIN32_LEAN_AND_MEAN

#ifndef  UNICODE

#define UNICODE

#endif

// Temporarily disable warnings in header files over which I have no control.
#pragma warning(push, 0)

#include <Windows.h>

#include <NTSecAPI.h>

#include <ntstatus.h>

#include <evntprov.h>

#include <stdio.h>

#pragma warning(pop)

#include "PassFiltEx.h"



REGHANDLE gEtwRegHandle;

HANDLE gBlacklistThread;

CRITICAL_SECTION gBlacklistCritSec;

BADSTRING* gBlacklistHead;

FILETIME gBlackListOldFileTime;

FILETIME gBlackListNewFileTime;

LARGE_INTEGER gPerformanceFrequency;

/*
DllMain
-------

https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583(v=vs.85).aspx

The only winning move is not to play.

*/
BOOL WINAPI DllMain(_In_ HINSTANCE DLLHandle, _In_ DWORD Reason, _In_ LPVOID Reserved)
{
	UNREFERENCED_PARAMETER(DLLHandle);

	UNREFERENCED_PARAMETER(Reason);
	
	UNREFERENCED_PARAMETER(Reserved);

	return(TRUE);
}


/*
InitializeChangeNotify
----------------------

The InitializeChangeNotify function is implemented by a password filter DLL. This function initializes the DLL.

Parameters:

None.

Return value:

TRUE
	The password filter DLL is initialized.

FALSE
	The password filter DLL is not initialized.

Remarks:

InitializeChangeNotify is called by the Local Security Authority (LSA) to verify that the password notification DLL is loaded and initialized.

This function must use the __stdcall calling convention, and must be exported by the DLL.

This function is called only for password filters that are installed and registered on a system.

Any process exception that is not handled within this function may cause security-related failures system-wide. Structured exception handling should be used when appropriate.

*/
__declspec(dllexport) BOOL CALLBACK InitializeChangeNotify(void)
{
	const GUID ETWProviderGuid = { 0x07d83223, 0x7594, 0x4852, { 0xba, 0xbc, 0x78, 0x48, 0x03, 0xfd, 0xf6, 0xc5 } };	
	
	if (EventRegister(&ETWProviderGuid, NULL, NULL, &gEtwRegHandle) != ERROR_SUCCESS)
	{
		return(FALSE);
	}

	EventWriteStringW2(L"[%s:%s@%d] ETW provider registered.", __FILENAMEW__, __FUNCTIONW__, __LINE__);

#pragma warning(push)
#pragma warning(disable: 6031) 
	// MSDN states that this call never fails so no need to bother with the return value.
	InitializeCriticalSectionAndSpinCount(&gBlacklistCritSec, 4000);
	
	// Same as above.
	QueryPerformanceFrequency(&gPerformanceFrequency);
#pragma warning(pop)	

	if ((gBlacklistThread = CreateThread(NULL, 0, BlacklistThreadProc, NULL, 0, NULL)) == NULL)
	{
		EventWriteStringW2(L"[%s:%s@%d] Failed to create blacklist update thread! Error 0x%08lx", __FILENAMEW__, __FUNCTIONW__, __LINE__, GetLastError());

		return(FALSE);
	}

	EventWriteStringW2(L"[%s:%s@%d] Blacklist update thread created.", __FILENAMEW__, __FUNCTIONW__, __LINE__);

	return(TRUE);
}

/*
PasswordChangeNotify
--------------------

The PasswordChangeNotify function is implemented by a password filter DLL. It notifies the DLL that a password was changed.

Parameters:

UserName [in]
	The account name of the user whose password changed.
	If the values of this parameter and the NewPassword parameter are NULL, this function should return STATUS_SUCCESS.

RelativeId [in]
	The relative identifier (RID) of the user specified in UserName.

NewPassword [in]
	A new plaintext password for the user specified in UserName. When you have finished using the password, clear the 
	information by calling the SecureZeroMemory function. For more information about protecting passwords, see Handling Passwords.
	If the values of this parameter and the UserName parameter are NULL, this function should return STATUS_SUCCESS.

Return value:

STATUS_SUCCESS
	Indicates the password of the user was changed, or that the values of both the UserName and NewPassword parameters are NULL.

Remarks:

The PasswordChangeNotify function is called after the PasswordFilter function has been called successfully and the new password has been stored.

This function must use the __stdcall calling convention and must be exported by the DLL.

When the PasswordChangeNotify routine is running, processing is blocked until the routine is finished. When appropriate, move any 
lengthy processing to a separate thread prior to returning from this routine.

This function is called only for password filters that are installed and registered on the system.

Any process exception that is not handled within this function may cause security-related failures system-wide.
Structured exception handling should be used when appropriate.

*/
__declspec(dllexport) NTSTATUS CALLBACK PasswordChangeNotify(_In_ PUNICODE_STRING UserName, _In_ ULONG RelativeId, _In_ PUNICODE_STRING NewPassword)
{
	UNREFERENCED_PARAMETER(UserName);

	UNREFERENCED_PARAMETER(RelativeId);

	UNREFERENCED_PARAMETER(NewPassword);

	// UNICODE_STRINGs are usually not null-terminated.
	// Let's make a null-terminated copy of it. sAMAccountNames can't be very long
	// so I think we're safe with this buffer size.

	wchar_t UserNameCopy[128] = { 0 };

	memcpy(&UserNameCopy, UserName->Buffer, UserName->Length);

	EventWriteStringW2(L"[%s:%s@%d] Password for %s (RID %lu) was changed.", __FILENAMEW__, __FUNCTIONW__, __LINE__, UserNameCopy, RelativeId);

	return(STATUS_SUCCESS);
}

/*
PasswordFilter
--------------

The PasswordFilter function is implemented by a password filter DLL. The value returned by this function determines whether the new password is accepted by the system. 
All of the password filters installed on a system must return TRUE for the password change to take effect.

Parameters:

AccountName [in]
	Pointer to a UNICODE_STRING that represents the name of the user whose password changed.

FullName [in]
	Pointer to a UNICODE_STRING that represents the full name of the user whose password changed.

Password [in]
	Pointer to a UNICODE_STRING that represents the new plaintext password. When you have finished using the password, clear it from memory by calling the SecureZeroMemory function.

SetOperation [in]
	TRUE if the password was set rather than changed.

Return value:

TRUE
	Return TRUE if the new password is valid with respect to the password policy implemented in the password filter DLL. 
	When TRUE is returned, the Local Security Authority (LSA) continues to evaluate the password by calling any other password filters installed on the system.

FALSE
	Return FALSE if the new password is not valid with respect to the password policy implemented in the password filter DLL.
	When FALSE is returned, the LSA returns the ERROR_ILL_FORMED_PASSWORD (1324) status code to the source of the password change request.

Remarks:

Password change requests may be made when users specify a new password, accounts are created and when administrators override a password.
This function must use the __stdcall calling convention and must be exported by the DLL.
When the PasswordFilter routine is running, processing is blocked until the routine is finished. When appropriate, move any lengthy processing to a separate thread prior to returning from this routine.
This function is called only for password filters that are installed and registered on a system.
Any process exception that is not handled within this function may cause security-related failures system-wide. Structured exception handling should be used when appropriate.

*/
__declspec(dllexport) BOOL CALLBACK PasswordFilter(_In_ PUNICODE_STRING AccountName, _In_ PUNICODE_STRING FullName, _In_ PUNICODE_STRING Password, _In_ BOOL SetOperation)
{
	UNREFERENCED_PARAMETER(FullName);

	BOOL PasswordIsOK = TRUE;

	LARGE_INTEGER StartTime = { 0 };

	LARGE_INTEGER EndTime = { 0 };

	LARGE_INTEGER ElapsedMicroseconds = { 0 };

	EnterCriticalSection(&gBlacklistCritSec);

	QueryPerformanceCounter(&StartTime);

	BADSTRING* CurrentNode = gBlacklistHead;

	// UNICODE_STRINGs are usually not null-terminated.
	// Let's make a null-terminated copy of it. sAMAccountNames can't be very long
	// so I think we're safe with this buffer size.

	wchar_t AccountNameCopy[128] = { 0 };

	memcpy(&AccountNameCopy, AccountName->Buffer, AccountName->Length);

	if (SetOperation)
	{
		EventWriteStringW2(L"[%s:%s@%d] SET password for user %s.", __FILENAMEW__, __FUNCTIONW__, __LINE__, AccountNameCopy);
	}
	else
	{
		EventWriteStringW2(L"[%s:%s@%d] CHANGE password for user %s.", __FILENAMEW__, __FUNCTIONW__, __LINE__, AccountNameCopy);
	}	

	wchar_t* PasswordCopy = NULL;
	
	if ((PasswordCopy = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Password->MaximumLength)) == NULL)
	{
		EventWriteStringW2(L"[%s:%s@%d] Error allocating memory! Cannot change password!", __FILENAMEW__, __FUNCTIONW__, __LINE__);

		PasswordIsOK = FALSE;

		goto End;
	}

	memcpy(PasswordCopy, Password->Buffer, Password->Length);

	for (unsigned int Counter = 0; Counter < wcslen(PasswordCopy) - 1; Counter++)
	{
		PasswordCopy[Counter] = towlower(PasswordCopy[Counter]);
	}	

	while (CurrentNode->Next != NULL)
	{
		CurrentNode = CurrentNode->Next;

		if (wcslen(CurrentNode->String) == 0)
		{
			EventWriteStringW2(L"[%s:%s@%d] ERROR: This blacklist token is 0 characters long. It will be skipped. Check your blacklist file for blank lines!", __FILENAMEW__, __FUNCTIONW__, __LINE__, CurrentNode->String);

			continue;
		}

		if (wcsstr(PasswordCopy, CurrentNode->String))
		{
			if (((float)wcslen(CurrentNode->String) / (float)wcslen(PasswordCopy)) >= 0.50f)
			{
				EventWriteStringW2(L"[%s:%s@%d] Rejecting password because it contains the blacklisted string \"%s\" and it is at least half of the full password!", __FILENAMEW__, __FUNCTIONW__, __LINE__, CurrentNode->String);

				PasswordIsOK = FALSE;

				break;
			}
		}
	}

	End:

	QueryPerformanceCounter(&EndTime);

	ElapsedMicroseconds.QuadPart = EndTime.QuadPart - StartTime.QuadPart;

	ElapsedMicroseconds.QuadPart *= 1000000;

	ElapsedMicroseconds.QuadPart /= gPerformanceFrequency.QuadPart;

	EventWriteStringW2(L"[%s:%s@%d] Finished in %llu microseconds.", __FILENAMEW__, __FUNCTIONW__, __LINE__, ElapsedMicroseconds.QuadPart);

	if (PasswordCopy != NULL)
	{
		SecureZeroMemory(PasswordCopy, Password->Length);

		HeapFree(GetProcessHeap(), 0, PasswordCopy);
	}	

	LeaveCriticalSection(&gBlacklistCritSec);

	return(PasswordIsOK);
}

DWORD WINAPI BlacklistThreadProc(_In_ LPVOID Args)
{
	UNREFERENCED_PARAMETER(Args);

	while (TRUE)
	{
		HANDLE BlacklistFileHandle = INVALID_HANDLE_VALUE;

		LARGE_INTEGER StartTime = { 0 };

		LARGE_INTEGER EndTime = { 0 };

		LARGE_INTEGER ElapsedMicroseconds = { 0 };

		EnterCriticalSection(&gBlacklistCritSec);

		QueryPerformanceCounter(&StartTime);

		// We are being loaded by lsass.exe. The current working directory of lsass should be C:\Windows\System32			

		if ((BlacklistFileHandle = CreateFile(BLACKLIST_FILENAME, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE)
		{
			EventWriteStringW2(L"[%s:%s@%d] Unable to open %s! Error 0x%08lx", __FILENAMEW__, __FUNCTIONW__, __LINE__, BLACKLIST_FILENAME, GetLastError());

			goto Sleep;
		}

		EventWriteStringW2(L"[%s:%s@%d] %s opened for read.", __FILENAMEW__, __FUNCTIONW__, __LINE__, BLACKLIST_FILENAME);

		if (GetFileTime(BlacklistFileHandle, NULL, NULL, &gBlackListNewFileTime) == 0)
		{
			EventWriteStringW2(L"[%s:%s@%d] Failed to call GetFileTime on %s! Error 0x%08lx", __FILENAMEW__, __FUNCTIONW__, __LINE__, BLACKLIST_FILENAME, GetLastError());

			goto Sleep;
		}

		if (CompareFileTime(&gBlackListNewFileTime, &gBlackListOldFileTime) != 0)
		{
			EventWriteStringW2(L"[%s:%s@%d] The last modified time of %s has changed since the last time we looked. Let's reload it.", __FILENAMEW__, __FUNCTIONW__, __LINE__, BLACKLIST_FILENAME);			

			// Initialize list head if we're here for the first time.
			if (gBlacklistHead == NULL)
			{
				if ((gBlacklistHead = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BADSTRING))) == NULL)
				{
					EventWriteStringW2(L"[%s:%s@%d] ERROR: Failed to allocate memory for list head!", __FILENAMEW__, __FUNCTIONW__, __LINE__);

					goto Sleep;
				}
			}

			// Need to clear blacklist and free memory first.
			BADSTRING* CurrentNode = gBlacklistHead;

			while (CurrentNode->Next != NULL)
			{
				CurrentNode = CurrentNode->Next;

				if (HeapFree(GetProcessHeap(), 0, CurrentNode) == 0)
				{
					EventWriteStringW2(L"[%s:%s@%d] HeapFree failed while clearing blacklist! Error 0x%08lx", __FILENAMEW__, __FUNCTIONW__, __LINE__, GetLastError());

					goto Sleep;
				}
			}

			// Create a new node for the first line of text in the file.
			if ((CurrentNode = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BADSTRING))) == NULL)
			{
				EventWriteStringW2(L"[%s:%s@%d] ERROR: Failed to allocate memory for list node!", __FILENAMEW__, __FUNCTIONW__, __LINE__);

				goto Sleep;
			}

			gBlacklistHead->Next = CurrentNode;

			DWORD TotalBytesRead = 0;

			DWORD BytesRead = 0;

			BYTE Read = 0;			

			DWORD BytesOnThisLine = 0;

			DWORD LinesRead = 1;

			while (TRUE)
			{
				if (ReadFile(BlacklistFileHandle, &Read, 1, &BytesRead, NULL) == FALSE)
				{
					break;
				}

				if (BytesRead == 0)
				{
					break;
				}

				if (BytesOnThisLine >= MAX_BLACKLIST_STRING_SIZE - 1)
				{
					EventWriteStringW2(L"[%s:%s@%d] WARNING: Line longer than max length of %d! Will truncate this line and attempt to resume reading the next line.", __FILENAMEW__, __FUNCTIONW__, __LINE__, MAX_BLACKLIST_STRING_SIZE);

					Read = 0x0A;
				}

				// Ignore unprintable characters
				if (Read < 0x20)
				{
					// Unless it's \n
					if (Read != 0x0A)
					{
						TotalBytesRead++;

						continue;
					}
				}

				if (Read == 0x0A)
				{
					BytesOnThisLine = 0;

					BADSTRING* NewNode = NULL;

					if ((NewNode = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BADSTRING))) == NULL)
					{
						EventWriteStringW2(L"[%s:%s@%d] ERROR: Failed to allocate memory for list node!", __FILENAMEW__, __FUNCTIONW__, __LINE__);

						goto Sleep;
					}

					CurrentNode->Next = NewNode;

					CurrentNode = NewNode;					

					TotalBytesRead++;

					LinesRead++;
					
					continue;
				}
				

				CurrentNode->String[BytesOnThisLine] = (wchar_t)towlower(Read);

				TotalBytesRead++;

				BytesOnThisLine++;
			}

			EventWriteStringW2(L"[%s:%s@%d] Read %lu bytes, %lu lines from file %s", __FILENAMEW__, __FUNCTIONW__, __LINE__, TotalBytesRead, LinesRead, BLACKLIST_FILENAME);
		}	

	Sleep:

		if (BlacklistFileHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(BlacklistFileHandle);
		}		

		gBlackListOldFileTime = gBlackListNewFileTime;

		QueryPerformanceCounter(&EndTime);

		ElapsedMicroseconds.QuadPart = EndTime.QuadPart - StartTime.QuadPart;
		
		ElapsedMicroseconds.QuadPart *= 1000000;
		
		ElapsedMicroseconds.QuadPart /= gPerformanceFrequency.QuadPart;

		EventWriteStringW2(L"[%s:%s@%d] Finished in %llu microseconds.", __FILENAMEW__, __FUNCTIONW__, __LINE__, ElapsedMicroseconds.QuadPart);

		LeaveCriticalSection(&gBlacklistCritSec);

		Sleep(BLACKLIST_THREAD_RUN_FREQUENCY);
	}

	return(0);
}

ULONG EventWriteStringW2(_In_ PCWSTR String, _In_ ...)
{
	wchar_t FormattedString[ETW_MAX_STRING_SIZE] = { 0 };

	va_list ArgPointer = NULL;

	va_start(ArgPointer, String);

	_vsnwprintf_s(FormattedString, sizeof(FormattedString) / sizeof(wchar_t), _TRUNCATE, String, ArgPointer);

	va_end(ArgPointer);

	return(EventWriteString(gEtwRegHandle, 0, 0, FormattedString));
}