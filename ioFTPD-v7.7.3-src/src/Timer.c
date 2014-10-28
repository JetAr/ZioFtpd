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

static CRITICAL_SECTION		csIoTimer;
static LPTIMER				*lpTimerList;
static HANDLE				hTimerUpdate;
static DWORD				dwHighTickCount, dwLowTickCount;
static DWORD				dwTimerListSize, dwTimerListItems;
static LONG volatile        lTimerShutdownLock;

__inline
VOID TimerGetTickCount(PUINT64 lpTickCount)
{
	DWORD	dwTickCount;

	//	Get current tickcount
	dwTickCount	= GetTickCount();
	//	Compare current tickcount to last (low) tickcount
	//z 这里是为了使用六十四位来计数
	if (dwTickCount < dwLowTickCount) dwHighTickCount++;

	dwLowTickCount	= dwTickCount;
	lpTickCount[0]	= (dwHighTickCount * 0x100000000) + dwLowTickCount;
}



BOOL ExecTimer(LPTIMER lpTimer)
{
	DWORD	dwResult;

	//	Set timer status, and handle stopped timer
	if (InterlockedExchange(&lpTimer->lStatus, TIMER_ACTIVE) != TIMER_QUEUED)
	{
		Free(lpTimer);
		return FALSE;
	}
	//	Execute timer procedure
	dwResult	= ((DWORD (__cdecl *)(LPVOID, LPTIMER))lpTimer->lpTimerProc)(lpTimer->lpTimerContext, lpTimer);
	//	Check if timer has been removed
	if (dwResult == INFINITE)
	{
		Free(lpTimer);
		return FALSE;
	}
	//	Restart timer, and validate it
	if (! dwResult ||
		! StartIoTimer(lpTimer, NULL, NULL, dwResult))
	{
		//	Deactivate
		InterlockedExchange(&lpTimer->lStatus, TIMER_INACTIVE);
	}

	return FALSE;
}


INT __cdecl TimerCompProc(LPTIMER *lpTimer1, LPTIMER *lpTimer2)
{
	if (lpTimer1[0]->ui64DueTime < lpTimer2[0]->ui64DueTime) return -1;
	return (lpTimer1[0]->ui64DueTime > lpTimer2[0]->ui64DueTime ? 1 : 0);
}



UINT WINAPI TimerThread(LPVOID lpVoid)
{
	LPTIMER		lpTimer, lpNTimer;
	UINT64		ui64;
	DWORD		dwResult, dwSleep, dwShift;

	dwSleep		= INFINITE;
	dwResult	= WAIT_OBJECT_0;
	//	Infinite loop
	for (;;)
	{
		EnterCriticalSection(&csIoTimer);
		if (dwTimerListItems)
		{
			TimerGetTickCount(&ui64);

			//z 处理已过约定时间的 timer
			if (ui64 >= lpTimerList[0]->ui64DueTime)
			{
				dwShift	= 0;
				do
				{
					lpTimer	= lpTimerList[dwShift++];
					for (;lpTimer;lpTimer = lpNTimer)
					{
						lpNTimer	= lpTimer->lpNext;
						//	Queue timer to external thread
						if (InterlockedExchange(&lpTimer->lStatus, TIMER_QUEUED) == TIMER_WAIT)
						{
							QueueJob(ExecTimer, lpTimer, JOB_PRIORITY_HIGH);						
						}
					}
					TimerGetTickCount(&ui64);

				} while (--dwTimerListItems && ui64 >= lpTimerList[dwShift]->ui64DueTime);

				//z 将剩下的timer向前移动
				MoveMemory(lpTimerList, &lpTimerList[dwShift], dwTimerListItems * sizeof(LPTIMER));
				//z 如果队列中还有剩余的 timer
				if (dwTimerListItems)
				{
					//z 计算到下次 timer 到达时间还得多久
					dwSleep	= (DWORD)(lpTimerList[0]->ui64DueTime <= ui64 ? 0 : lpTimerList[0]->ui64DueTime - ui64);
				}

				else dwSleep	= INFINITE;
			}
			else dwSleep	= (DWORD)(lpTimerList[0]->ui64DueTime <= ui64 ? 0 : lpTimerList[0]->ui64DueTime - ui64);
		}
		else dwSleep	= INFINITE;

		//	Cancel waitable timer
		//z 对数组处理完毕
		LeaveCriticalSection(&csIoTimer);

		//z 使用这个避免了不必要的循环（从而不需要消耗cpu时间）
		WaitForSingleObject(hTimerUpdate, dwSleep);
		//z 查看 dwDaemonStatus，查看是否为 shutdown 状态
		if (dwDaemonStatus == DAEMON_SHUTDOWN)
		{
			break;
		}
	}
	//z dwDaemonStatus
	InterlockedExchange(&lTimerShutdownLock, TRUE);

	//	Exit from this thread
	CloseHandle(hTimerUpdate);
	Free(lpTimerList);
	DeleteCriticalSection(&csIoTimer);
	ExitThread(0);

	return FALSE;
}



BOOL DeleteIoTimer(LPTIMER lpTimer)
{
	LPTIMER		*lpResult, lpLastTimer, lpCTimer;
	DWORD		dwBytesToMove;

	//	Search for timer (should always exist)
	//z 按照 duiTime 顺序存放
	lpResult	= (LPTIMER *)bsearch(&lpTimer, lpTimerList, dwTimerListItems,
		                             sizeof(LPTIMER), (QUICKCOMPAREPROC) TimerCompProc);

	if (!lpResult)
	{
		Putlog(LOG_ERROR, _T("Failed to find timer being deleted.\r\n"));
		return FALSE;
	}

	//	Copy result to proper type variable
	if (lpResult[0]->lpNext)
	{
		lpLastTimer	= NULL;
		//	Find timer
		for (lpCTimer = lpResult[0];lpCTimer;lpCTimer = lpCTimer->lpNext)
		{
			//z 写得稍微复杂点儿是为了处理链表的删除。
			//	Compare timers
			if (lpCTimer == lpTimer)//z 如果相等，说明当前这个就是查找的那个timer
			{
				//	Compare against first timer
				if (! lpLastTimer)//z 第一次 lpLastTimer 为 NULL。说明第一个就与 lpTimer 相等。
				{
					//z 将 Next timer 放置到 lpResult 中去。
					lpResult[0]	= lpTimer->lpNext;
				}
				//z 其次则将后续lpLastTimer->Next 指向到 lpTimer 的 next timer 。
				else lpLastTimer->lpNext	= lpTimer->lpNext;
				//	Break from the loop
				break;
			}
			lpLastTimer	= lpCTimer;
		}
	}
	else
	{
		//z 如果该 timer 没有 next，直接移动后面的 status 。
		//	Calculate number of bytes to move
		dwBytesToMove	= (ULONG)&lpTimerList[--dwTimerListItems] - (ULONG)lpResult;
		//	Remove item from list
		MoveMemory(lpResult, &lpResult[1], dwBytesToMove);
	}
	return FALSE;
}



BOOL AllocateTimerArray(VOID)
{
	LPVOID	lpMemory;
	//	Allocate more memory for next timer
	//z 初始的时候 dwTimerListItems 为0。每次增加1000，为啥不用 1024 了。
	if (! (lpMemory = ReAllocate(lpTimerList, "Timer:Array", (dwTimerListItems + 1000) * sizeof(LPTIMER)))) return TRUE;
	dwTimerListSize	+= 1000;
	lpTimerList	= (LPTIMER *)lpMemory;
	return FALSE;
}


LPTIMER StartIoTimer(LPTIMER lpTimer, LPVOID lpTimerProc, LPVOID lpTimerContext, DWORD dwTimeOut)
{
	LPTIMER			lpTimer2;
	INT				iResult;

	//z 如果 lpTimer 为 null，那么新生成一个。
	if (! lpTimer)
	{
		//	Allocate memory for timer structure
		lpTimer2	= (LPTIMER)Allocate("Timer", sizeof(TIMER));
		if (! lpTimer2) return NULL;
		//	Store some variables
		lpTimer2->lStatus		= TIMER_WAIT;
		lpTimer2->lpTimerProc	= lpTimerProc;
		lpTimer2->lpTimerContext	= lpTimerContext;
	}
	else lpTimer2	= lpTimer;//z 直接等于

	//z 对队列进行操作，需要 thread-safty
	EnterCriticalSection(&csIoTimer);
	//	Due time
	TimerGetTickCount(&lpTimer2->ui64DueTime);
	//z 如果不是 100 的整数，将该数变成100的倍数
	if ((iResult = (INT)(lpTimer2->ui64DueTime += dwTimeOut) % 100))
	{
		lpTimer2->ui64DueTime	+= (100 - iResult);
	}

	//	Sanity check
	//z 已满，那么分配新的空间。
	if (dwTimerListItems == dwTimerListSize &&
		AllocateTimerArray())
	{
		LeaveCriticalSection(&csIoTimer);
		//	Free memory
		if (! lpTimer) Free(lpTimer2);
		return NULL;
	}
	
	//	Insert timer to timer array
	//z 将之快速插入到队列中去
	iResult	= QuickInsert(lpTimerList, dwTimerListItems, lpTimer2, (QUICKCOMPAREPROC) TimerCompProc);

	if (iResult--)
	{
		//	Similar timer exists, append
		lpTimer2->lpNext			= lpTimerList[iResult];
		lpTimerList[iResult]	= lpTimer2;
	}
	else
	{
		//	New item in array
		if (++dwTimerListItems == dwTimerListSize) AllocateTimerArray();
		lpTimer2->lpNext	= NULL;

		//	Reset timer, if timer is first in queue
		if (lpTimerList[0] == lpTimer2)
		{
			if (!SetEvent(hTimerUpdate))
			{
				iResult = GetLastError();
			}
		}
	}
	if (lpTimer) InterlockedExchange(&lpTimer2->lStatus, TIMER_WAIT);
	LeaveCriticalSection(&csIoTimer);

	return lpTimer2;
}




BOOL StopIoTimer(LPTIMER lpTimer, BOOL bSkipActive)
{
	//	Sanity check
	if (!lpTimer) return 0;

	for (;;)
	{
		switch (InterlockedExchange(&lpTimer->lStatus, TIMER_CANCEL))
		{
		case TIMER_CANCEL:
		case TIMER_ACTIVE:
			//	Timer procedure is currently active
			if (bSkipActive) return -1;
			SwitchToThread();
			break;
		case TIMER_WAIT:
			//	Timer is in wait state
			EnterCriticalSection(&csIoTimer);
			//	Remove timer if it has not been posted
			if (lpTimer->lStatus != TIMER_QUEUED) DeleteIoTimer(lpTimer);
			LeaveCriticalSection(&csIoTimer);
			Free(lpTimer);
			return 0;
		case TIMER_INACTIVE:
			//	Timer is inactive
			Free(lpTimer);
			return 1;
		case TIMER_QUEUED:
			return 0;
		}
	}
}



BOOL Timer_Init(BOOL bFirstInitialization)
{
	HANDLE	hTimerThread;
	DWORD	dwThreadId;

	//z 不是第一次初始化，直接返回。
	if (! bFirstInitialization) return TRUE;
	
	//	Reset local variables
	dwHighTickCount		= 0;
	dwLowTickCount		= 0;
	dwTimerListSize		= 0;
	dwTimerListItems	= 0;
	lpTimerList			= NULL;
	//	Initialize critical section 初始化 CRITICAL SECTION
	InitializeCriticalSectionAndSpinCount(&csIoTimer, 100);
	//	Create event 
	//z 参数依次为安全属性，是否自动重置，初始为non-signaled，第四个为名字，这里创建匿名event。
	hTimerUpdate	= CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hTimerUpdate == INVALID_HANDLE_VALUE) return FALSE;
	
	//	Allocate memory
	//z 先分配 1000 个
	if (AllocateTimerArray()) return FALSE;
	//	Create timer thread
	//z 创建一个 timer 线程
	hTimerThread	= CreateThread(NULL, 0,
		(LPTHREAD_START_ROUTINE)TimerThread, 0, 0, &dwThreadId);
	//	Validate thread handle
	if (hTimerThread == INVALID_HANDLE_VALUE) return FALSE;
	//	Set thread priority to high!!
	SetThreadPriority(hTimerThread, THREAD_PRIORITY_ABOVE_NORMAL);
	CloseHandle(hTimerThread);

	return TRUE;
}


VOID Timer_DeInit(VOID)
{
	DWORD dwError;

	//	Free resources
	if (!InterlockedExchange(&lTimerShutdownLock, TRUE))
	{
		if (!SetEvent(hTimerUpdate))
		{
			// for debugging...
			dwError = GetLastError();
		}
		SwitchToThread();
	}
}
