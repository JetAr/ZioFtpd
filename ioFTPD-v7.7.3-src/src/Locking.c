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

BOOL InitializeLockObject(LPLOCKOBJECT lpLockObject)
{
    lpLockObject->lExclusive	= FALSE;

    /*CreateEventA(
    __in_opt LPSECURITY_ATTRIBUTES lpEventAttributes,
    __in     BOOL bManualReset,
    __in     BOOL bInitialState,
    __in_opt LPCSTR lpName
    );*/
    
    //z 创建一个手动重置事件，初始状态为 signaled
    lpLockObject->hEvent[0]	= CreateEvent(NULL, TRUE, TRUE, NULL);
    //z 检查是否创建成功
    if (lpLockObject->hEvent[0] == INVALID_HANDLE_VALUE) return FALSE;
    //z 信号量 10000
    lpLockObject->hEvent[1]	= CreateSemaphore(NULL, LOCK_MAX_COUNT, LOCK_MAX_COUNT, 0);

    //z 检查是否创建成功
    if (!lpLockObject->hEvent[1])
    {
        CloseHandle(lpLockObject->hEvent[0]);
        lpLockObject->hEvent[0] = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    return TRUE;
}

//z 删除 LockObject
VOID DeleteLockObject(LPLOCKOBJECT lpLockObject)
{
    //z 关闭事件句柄
    HANDLE hEvent;

    hEvent = lpLockObject->hEvent[0];
    if (hEvent && (hEvent != INVALID_HANDLE_VALUE))
    {
        CloseHandle(hEvent);
    }
    lpLockObject->hEvent[0] = INVALID_HANDLE_VALUE;

    hEvent = lpLockObject->hEvent[1];
    if (hEvent && (hEvent != INVALID_HANDLE_VALUE))
    {
        CloseHandle(hEvent);
    }
    lpLockObject->hEvent[1] = INVALID_HANDLE_VALUE;
}

//z 获取共享锁
VOID AcquireSharedLock(LPLOCKOBJECT lpLockObject)
{
    //z 在两个事件上等待
    //z 等待成功后会怎样？
    WaitForMultipleObjects(2, lpLockObject->hEvent, TRUE, INFINITE);
}

VOID ReleaseSharedLock(LPLOCKOBJECT lpLockObject)
{
    ReleaseSemaphore(lpLockObject->hEvent[1], 1, NULL);
}

//z 获取排它锁
VOID AcquireExclusiveLock(LPLOCKOBJECT lpLockObject)
{
    LONG	lCount, lLeft;

    //z 返回 lpLockObject->lExcolusive 的初始值。
    while (InterlockedExchange(&lpLockObject->lExclusive, TRUE)) WaitForSingleObject(lpLockObject->hEvent[0], INFINITE);
    ResetEvent(lpLockObject->hEvent[0]);
    for (;;)
    {
        WaitForSingleObject(lpLockObject->hEvent[1], INFINITE);
        ReleaseSemaphore(lpLockObject->hEvent[1], 1, &lCount);
        lLeft	= (LOCK_MAX_COUNT - 1) - lCount;
        if (! lLeft) break;
        if (lLeft < 10)
        {
            SwitchToThread();
        }
        else Sleep(10);
    }
}

VOID ReleaseExclusiveLock(LPLOCKOBJECT lpLockObject)
{
    SetEvent(lpLockObject->hEvent[0]);
    InterlockedExchange(&lpLockObject->lExclusive, FALSE);
}

/*
BOOL InitializeLockObject(LPLOCKOBJECT lpLock)
{
	//	Reset memory
	ZeroMemory(lpLock, sizeof(LOCKOBJECT));
	//	Create event
	lpLock->hEvent	= CreateEvent(NULL, TRUE, TRUE, NULL);
	//	Validate event
	return (lpLock->hEvent == INVALID_HANDLE_VALUE ? TRUE : FALSE);
}


VOID DeleteLockObject(LPLOCKOBJECT lpLock)
{
	//	Close event
	if (lpLock->hEvent != INVALID_HANDLE_VALUE) CloseHandle(lpLock->hEvent);
}


VOID AcquireSharedLock(LPLOCKOBJECT lpLock)
{
	for (;;)
	{
		//	Wait until lock is granted
		while (InterlockedExchange(&lpLock->lLock, TRUE)) SwitchToThread();
		//	Break from loop
		if (! lpLock->bExclusiveLock) break;
		//	Release lock
		InterlockedExchange(&lpLock->lLock, FALSE);
		//	Wait until event is signaled
		WaitForSingleObject(lpLock->hEvent, INFINITE);
	}
	//	Increase access counter
	InterlockedIncrement(&lpLock->lCounter);
	//	Release lock
	InterlockedExchange(&lpLock->lLock, FALSE);
}


VOID ReleaseSharedLock(LPLOCKOBJECT lpLock)
{
	//	Decrease access counter
	InterlockedDecrement(&lpLock->lCounter);
}



VOID AcquireExclusiveLock(LPLOCKOBJECT lpLock)
{
	for (;;)
	{
		//	Wait until lock is granted
		while (InterlockedExchange(&lpLock->lLock, TRUE)) SwitchToThread();
		//	Break from loop
		if (! lpLock->bExclusiveLock) break;
		//	Release lock
		InterlockedExchange(&lpLock->lLock, FALSE);
		//	Wait until event is signaled
		WaitForSingleObject(lpLock->hEvent, INFINITE);
	}
	//	Reset event
	ResetEvent(lpLock->hEvent);
	//	Enable exclusive lock
	lpLock->bExclusiveLock	= TRUE;
	//	Release lock
	InterlockedExchange(&lpLock->lLock, FALSE);
	//	Wait until counter hits zero
	while (lpLock->lCounter) SwitchToThread();
}


VOID ReleaseExclusiveLock(LPLOCKOBJECT lpLock)
{
	//	Release exclusive lock
	lpLock->bExclusiveLock	= FALSE;
	//	Release waiting threads
	SetEvent(lpLock->hEvent);
}

*/
