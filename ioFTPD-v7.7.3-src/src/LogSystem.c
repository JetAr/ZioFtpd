/*
 * Copyright(c) 2006 iniCom Networks, Inc.
 *
 * This file is part of ioFTPD.
 *
 * ioFTPD is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ioFTPD is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ioFTPD; see the file COPYING.  if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <ioFTPD.h>


// LOG <timestamp> <whata> <who> <arg1> <arg2>
//
//		WHAT	WHO							ARG1	ARG2
//
//		LOGIN	 "<user>:<group>:<tagline>" "<host>" "<type>"			<done ftp/telnet, host not ok yet>
//		LOGOUT	 "<user>:<group>:<tagline>" "<host>" "<type>:<reason>"	<done ftp/telnet, host/reason not ok yet>
//		NEWDIR	 "<user>:<group>" "<path>"								<done>
//		DELDIR	 "<user>:<group>" "<path>"								<done>
//		ADDUSER	 "<user>" "<user>[:<group>]"
//		RENUSER  "<user>" "<user>"
//		DELUSER  "<user>" "<user>"
//		GRPADD	 "<user>" "<group>:<group long name>"
//		GRPREN	 "<user>" "<group>:<group long name>" "<group>"
//		GRPDEL	 "<user>" "<group>:<group long name>"
//		ADDIP	 "<user>" "<user>" "<ip>"
//		DELIP	 "<user>" "<user>" "<ip>"
//		CHANGE	 "<user>" "<user>" "<change what>:<old value>:<new value>"
//		KICK	 "<user>" "<user>" "<reason>"



static SYSTEMTIME			LocalTime;
static UINT64				FileTime[2];
static volatile LPLOG_VECTOR		lpFirstLogItem, lpLastLogItem, lpLogWriteQueue;
static DWORD				dwLogVectorLength, dwLogVectorHistory;
static CRITICAL_SECTION	csLogLock;
static volatile LONG        lQueueLogEntries;
static LPTSTR               tszLogFileNameArray[LOG_TOTAL_TYPES];

BOOL
LogSystem_Init(BOOL bFirstInitialization)
{
    //z 如果已经初始化过了，那么直接返回
    if (! bFirstInitialization) return TRUE;

    //z 读取各种情况对应的 Log 文件路径
    tszLogFileNameArray[LOG_ERROR]    = Config_Get_Path(&IniConfigFile, _TEXT("Locations"), _TEXT("Log_Files"), _TEXT("Error.log"), NULL);
    tszLogFileNameArray[LOG_TRANSFER] = Config_Get_Path(&IniConfigFile, _TEXT("Locations"), _TEXT("Log_Files"), _TEXT("xferlog"), NULL);
    tszLogFileNameArray[LOG_SYSOP]    = Config_Get_Path(&IniConfigFile, _TEXT("Locations"), _TEXT("Log_Files"), _TEXT("SysOp.log"), NULL);
    tszLogFileNameArray[LOG_GENERAL]  = Config_Get_Path(&IniConfigFile, _TEXT("Locations"), _TEXT("Log_Files"), _TEXT("ioFTPD.log"), NULL);
    tszLogFileNameArray[LOG_SYSTEM]   = Config_Get_Path(&IniConfigFile, _TEXT("Locations"), _TEXT("Log_Files"), _TEXT("SystemError.log"), NULL);
    tszLogFileNameArray[LOG_DEBUG]    = Config_Get_Path(&IniConfigFile, _TEXT("Locations"), _TEXT("Log_Files"), _TEXT("Debug.log"), NULL);

    // Reset vector
    lpFirstLogItem		= NULL;
    lpLastLogItem		= NULL;
    lpLogWriteQueue		= NULL;
    dwLogVectorLength	= 0;
    dwLogVectorHistory	= 0;
    ZeroMemory(&FileTime, sizeof(UINT64) * 2);

    //	Initialize vector lock
    if (! InitializeCriticalSectionAndSpinCount(&csLogLock, 50)) return FALSE;

    return TRUE;
}

//z 这部分没看太明白，是如何将剩下的内容给写入到文件中去了？通过出让时间？
VOID LogSystem_Flush()
{
    DWORD dwTickCount;

    // make sure existing log writes are flushed
    EnterCriticalSection(&csLogLock);
    dwTickCount = GetTickCount() + 1000;
    while (lpLogWriteQueue && GetTickCount() < dwTickCount)
    {
        LeaveCriticalSection(&csLogLock);
        //z 100 ms，
        SleepEx(100, TRUE);
        EnterCriticalSection(&csLogLock);
    }
    LeaveCriticalSection(&csLogLock);
}

VOID LogSystem_DeInit()
{
    LPLOG_VECTOR	lpLogItem;
    DWORD n;

    Putlog(LOG_GENERAL, _TEXT("STOP: \"PID=%u\"\r\n"), GetCurrentProcessId());

    // make sure log writes are flushed
    LogSystem_Flush();

    //	Delete vector lock
    DeleteCriticalSection(&csLogLock);
    //	Free memory
    for (; lpLogItem = lpFirstLogItem;)
    {
        lpFirstLogItem	= lpLogItem->lpNext;
        Free(lpLogItem);
    }

    for(n=0; n<LOG_TOTAL_TYPES; n++)
    {
        Free(tszLogFileNameArray[n]);
    }
}


VOID LogSystem_NoQueue(VOID)
{
    // make new log entries print directly to file
    //z 不使用 Queue 的方式，直接将log写入到file中去
    lQueueLogEntries = 0;

    // make sure log writes are flushed
    //z 将日志中剩下的内容给 Flush 到文件的
    LogSystem_Flush();
}


BOOL
LogSystem_Queue(BOOL bFirstInitialization)
{
    if (! bFirstInitialization) return TRUE;

    lQueueLogEntries = 1;
    return TRUE;
}

//z 格式化 Log Entry 的 Format。
//z 格式化时间，将时间和文件内容写入到文件中去。
VOID FormatLogEntry(LPLOG_VECTOR lpLogItem)
{
    SYSTEMTIME   SystemTime;
    HANDLE       FileHandle;
    DWORD		 dwBytesWritten, dwTimeBufferLength;
    TCHAR		 tpTimeBuffer[128];

    //	Get filehandle
    //z previous 用于存储log file 的 handle
    FileHandle	= (HANDLE)lpLogItem->lpPrevious;

    if (FileHandle != INVALID_HANDLE_VALUE)
    {
        //	Convert filetime to systemtime
        CopyMemory(&FileTime[0], &lpLogItem->FileTime, sizeof(UINT64));
        //z 大于这个值，有什么用意了？如果小于这个值，在后面用到的格式里是不能区分出来的？差别小于秒？
        //z 这样能够减少操作的次数
        if (FileTime[0] - FileTime[1] > 10000000)
        {
            FileTime[1]	= FileTime[0];
            FileTimeToSystemTime(&lpLogItem->FileTime, &SystemTime);
            SystemTimeToLocalTime(&SystemTime, &LocalTime);
        }

        //	Seek to end of file
        //z 到达文件的结尾
        SetFilePointer(FileHandle, 0, 0, FILE_END);
        //	Format timestamp
        if (lpLogItem->dwLogCode != LOG_TRANSFER)
        {
            dwTimeBufferLength	= wsprintf(tpTimeBuffer, _TEXT("%02d-%02d-%04d %02d:%02d:%02d "),
                                           LocalTime.wMonth, LocalTime.wDay, LocalTime.wYear,
                                           LocalTime.wHour, LocalTime.wMinute, LocalTime.wSecond);
        }
        else
        {
            //z LOG_TRANSFER 时，特别处理了下，其格式与其他地方不一样
            dwTimeBufferLength	= wsprintf(tpTimeBuffer, _TEXT("%.3s %.3s %2d %02d:%02d:%02d %4d "),
                                           WeekDays[LocalTime.wDayOfWeek], Months[LocalTime.wMonth], LocalTime.wDay,
                                           LocalTime.wHour, LocalTime.wMinute, LocalTime.wSecond, LocalTime.wYear);
        }

        //	Write buffers to file
        //z 先写下时间，然后写下记录的内容
        if (! WriteFile(FileHandle, tpTimeBuffer, dwTimeBufferLength * sizeof(TCHAR), &dwBytesWritten, NULL) ||
                ! WriteFile(FileHandle, lpLogItem->lpMessage, lpLogItem->dwMessageLength, &dwBytesWritten, NULL))
        {
            //	Write failed
        }
        //z 关闭文件句柄
        CloseHandle(FileHandle);
    }
}




BOOL WriteLog(LPLOG_VECTOR lpQueueOffset)
{
    LPLOG_VECTOR	lpLogItem;

    for (; (lpLogItem = lpQueueOffset);)
    {
        FormatLogEntry(lpLogItem);

        EnterCriticalSection(&csLogLock);
        //	Pop list
        lpQueueOffset	= lpQueueOffset->lpNext;
        if (! lpQueueOffset) lpLogWriteQueue	= NULL;

        //	Add item to log vector
        dwLogVectorLength++;
        dwLogVectorHistory++;

        if (lpLastLogItem)
        {
            //	Append to vector
            lpLogItem->lpNext		= NULL;
            lpLogItem->lpPrevious	= lpLastLogItem;
            lpLastLogItem->lpNext	= lpLogItem;
            lpLastLogItem			= lpLogItem;
            //	Check if log limit has been reached
            if (dwLogVectorLength > MAX_LOGS)
            {
                //	Remove first item from memory
                lpFirstLogItem	= lpFirstLogItem->lpNext;
                Free(lpFirstLogItem->lpPrevious);
                lpFirstLogItem->lpPrevious	= NULL;
                dwLogVectorLength--;
            }
        }
        else
        {
            //	Vector is empty
            lpLogItem->lpNext		= NULL;
            lpLogItem->lpPrevious	= NULL;
            lpFirstLogItem			= lpLogItem;
            lpLastLogItem			= lpLogItem;
        }
        LeaveCriticalSection(&csLogLock);
    }
    return FALSE;
}

BOOL PutlogVA(DWORD dwLogCode, LPCTSTR tszFormatString, va_list Arguments)
{
    LPLOG_VECTOR	lpLogItem;
    LPTSTR			tszFileName;

    //	Allocate memory
    //z 分配存储空间
    lpLogItem	= (LPLOG_VECTOR)Allocate("Log", sizeof(LOG_VECTOR));
    if (! lpLogItem) return TRUE;

    //	Initialize item contents
    //z 将系统的utc时间转化为文件时间
    GetSystemTimeAsFileTime(&lpLogItem->FileTime);
    lpLogItem->lpNext		= NULL;
    //z 设置对应的code
    lpLogItem->dwLogCode	= dwLogCode;
    //	Format string to buffer, don't care if NULL terminated since we printing buffer based on length...
    //z 根据字符串格式得到对应的字符串
    lpLogItem->dwMessageLength	= _vsntprintf(lpLogItem->lpMessage, LOG_LENGTH, tszFormatString, Arguments);
    //	Check length for overflow
    //z 返回-1的话，实际长度是超过了当前给出的size的。
    if (lpLogItem->dwMessageLength == (DWORD)-1)
    {
        // make sure it ends in \r\n so next message doesn't appear on same line
        //z 如果最终是超过了的，设置为最大字符串
        lpLogItem->dwMessageLength	= LOG_LENGTH;
        //z 并且在字符串尾部加上 回车、换行
        lpLogItem->lpMessage[LOG_LENGTH-2] = _T('\r');
        lpLogItem->lpMessage[LOG_LENGTH-1] = _T('\n');
    }

    //z 查验code
    if (dwLogCode < LOG_TOTAL_TYPES)
    {
        tszFileName = tszLogFileNameArray[dwLogCode];
    }
    else
    {
        tszFileName = NULL;
    }

    //	Open logfile
    if (tszFileName)
    {
        //z 将 lpPrevious 设置为文件的 handle 。
        lpLogItem->lpPrevious	= (LPLOG_VECTOR)CreateFile(tszFileName, GENERIC_WRITE,
                                  FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    }
    else lpLogItem->lpPrevious	= INVALID_HANDLE_VALUE;

    // hold lock to order events during shutdown
    EnterCriticalSection(&csLogLock);
    if (lQueueLogEntries)
    {
        //	Add item to write queue
        //z 如果此时 lpLogWriteQueue 为 null
        //z 依据是否是第一个，为加入queue采用不同的方式
        if (! lpLogWriteQueue)
        {
            //	New write queue
            //z 设置为第一个
            lpLogWriteQueue	= lpLogItem;
            //z 将 lpLogItem 加到 WriteLog 上去。
            QueueJob(WriteLog, lpLogItem, JOB_PRIORITY_LOW);
        }
        else
        {
            //	Append to write queue
            //z 将 lpLogItem 加入到 lpLogWriteQueue
            lpLogWriteQueue->lpNext	= lpLogItem;
            //z 将 lpLogItem 设置为最后一个
            lpLogWriteQueue	= lpLogItem;
        }
    }
    else
    {
        // startup/shutdown... don't queue stuff
        //z 开始或是关闭的时候，不queue stuff
        if (lpLogItem->lpPrevious)
        {
            //z 如果文件句柄良好；那么存储log内容到文件
            FormatLogEntry(lpLogItem);
        }
        Free(lpLogItem);
    }
    LeaveCriticalSection(&csLogLock);

    return FALSE;
}


BOOL Putlog(DWORD dwLogCode, LPCTSTR tszFormatString, ...)
{
    BOOL     bReturn;
    va_list	 Arguments;

    va_start(Arguments, tszFormatString);
    bReturn = PutlogVA(dwLogCode, tszFormatString, Arguments);
    va_end(Arguments);

    return bReturn;
}
