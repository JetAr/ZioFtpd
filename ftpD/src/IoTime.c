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

static INT64	i64TicksPerSecond;//z 缓存 frequency of performance counter
static TIME_ZONE_INFORMATION	TimeZoneInformation;

//z 系统时间转换为本地时间
VOID SystemTimeToLocalTime(LPSYSTEMTIME lpUniversalTime, LPSYSTEMTIME lpLocalTime)
{
    //z 将utc时间转换为本地时间
    if (! SystemTimeToTzSpecificLocalTime(&TimeZoneInformation, lpUniversalTime, lpLocalTime))
    {
        //z 失败的话，返回原时间
        CopyMemory(lpLocalTime, lpUniversalTime, sizeof(SYSTEMTIME));
    }
}

//z 比较时间
INT Time_Compare(LPTIME_STRUCT lpStartTime, LPTIME_STRUCT lpStopTime)
{
    DWORD	dwTickCount;

    //z 如果支持高精度
    if (i64TicksPerSecond)
    {
        if (lpStartTime->i64TickCount > lpStopTime->i64TickCount) return 1;
        if (lpStartTime->i64TickCount < lpStopTime->i64TickCount) return -1;
    }
    else
    {
        dwTickCount	= GetTickCount();
        //	Check timer 1wrapping
        if (lpStartTime->i64TickCount > dwTickCount)
        {
            //	Check timer2 wrapping
            if (lpStopTime->i64TickCount < dwTickCount) return 1;
        }
        else if (lpStopTime->i64TickCount > dwTickCount) return -1;

        if (lpStartTime->i64TickCount > lpStopTime->i64TickCount) return 1;
        if (lpStartTime->i64TickCount < lpStopTime->i64TickCount) return -1;
    }
    return 0;
}



DOUBLE Time_Difference(LPTIME_STRUCT lpStartTime, LPTIME_STRUCT lpStopTime)
{
    if (i64TicksPerSecond)
    {
        //z 返回秒数
        return (lpStopTime->i64TickCount - lpStartTime->i64TickCount) / 1. / i64TicksPerSecond;
    }
    return (lpStartTime->i64TickCount > lpStopTime->i64TickCount ?
            (0xFFFFFFFFL - lpStartTime->i64TickCount) + 1 + lpStopTime->i64TickCount : lpStopTime->i64TickCount - lpStartTime->i64TickCount) / 1000.;
}



DWORD Time_DifferenceDW32(DWORD dwStart, DWORD dwStop)
{
    /*
    dwStart < dwStop 时，直接返回 dwStop - dwStart 。
    如果dwStop比dwStart小，说明dwStop可能溢出了？
    这里考虑了：假设只溢出一次
    */
    return (dwStart > dwStop ?
            (0xFFFFFFFFL - dwStart) + 1 + dwStop : dwStop - dwStart);
}


VOID Time_Read(LPTIME_STRUCT lpTime)
{
    if (i64TicksPerSecond)
    {
        //z 得到当前的 i64TickCount 。
        QueryPerformanceCounter((PLARGE_INTEGER)&lpTime->i64TickCount);
    }
    else lpTime->i64TickCount	= GetTickCount();
}

//z 写得比较费解；按照一定的格式输出时间，比如 "1 year 3 week" 之类的？
VOID Time_Duration(LPTSTR tszBuf, DWORD dwSize, time_t tDiff, TCHAR cLast, TCHAR cFirst,
                   DWORD dwSuffixType, DWORD dwShowZeros, DWORD dwMinWidth, LPTSTR tszField)
{
    DWORD		n, tTemp, tTime;
    INT			i, iResult;
    DWORD       pdwList[]    = { 31536000, 604800,  86400,  3600,    60,     1,      0 };
    LPSTR       pszSuffix[]  = { "y",      "w",     "d",    "h",     "m",    "s",    0 };
    LPSTR       pszLongSuf[] = { " year",  " week", " day", " hour", " min", " sec", 0 };
    BOOL        bFirst;
    LPTSTR      tszSpace, *tszSuffixArray;

    tszBuf[0] = 0;
    i = 0;
    bFirst = TRUE;
    tTime = (DWORD) tDiff;
    if (!tszField) tszField = _T(" ");
    if ((dwSuffixType == 2) && dwMinWidth)
    {
        tszSpace = _T(" ");
    }
    else
    {
        tszSpace = _T("");
    }

    //z 使用长格式还是短格式。
    if (dwSuffixType)
    {
        tszSuffixArray = pszLongSuf;
    }
    else
    {
        tszSuffixArray = pszSuffix;
    }

    //z
    for ( n=0 ; pdwList[n] ; n++ )
    {
        // skip until start position
        if (cFirst)
        {
            if (cFirst != *pszSuffix[n]) continue;
            cFirst = 0;
        }

        tTemp = tTime / pdwList[n];

        if (tTemp || (dwShowZeros && !bFirst))
        {
            bFirst = FALSE;
            iResult = _snprintf_s(&tszBuf[i], dwSize - i, _TRUNCATE, _T("%s%*d%s%s"),
                                  (i == 0 ? _T("") : tszField), dwMinWidth, tTemp, tszSuffixArray[n],
                                  (dwSuffixType==2 && tTemp>1 ? _T("s") : tszSpace));
            if (iResult  < 0) break;
            i += iResult;
        }

        if (cLast == *pszSuffix[n]) break;

        tTime = tTime % pdwList[n];
    }

    if (!tszBuf[0])
    {
        if (!cLast)
        {
            n = 5;
        }
        else
        {
            for ( n=0 ; pdwList[n] ; n++ )
            {
                if (cLast == *pszSuffix[n]) break;
            }
            if (!pdwList[n]) n--;
        }
        _snprintf_s(tszBuf, dwSize, _TRUNCATE, "%*d%s", dwMinWidth, 0, tszSuffixArray[n]);
    }
}

//z 初始化，主要查看硬件是否高精度计数器，支持的话，将frequency of the performance counter缓存起来。
BOOL Time_Init(BOOL bFirstInitialization)
{
    if (! bFirstInitialization) return TRUE;
    //	Get time zone 获取 time zone 信息
    if (GetTimeZoneInformation(&TimeZoneInformation) == TIME_ZONE_ID_INVALID)
    {
        //z 如果获取失败，使用 0 来填充
        ZeroMemory(&TimeZoneInformation, sizeof(TIME_ZONE_INFORMATION));
    }

    //z Retrieves the frequency of the performance counter。每秒的 counts。
    //z 如果硬件不支持高精度，将该参数设置为0。支持，返回非零值。
    if (! QueryPerformanceFrequency((PLARGE_INTEGER)&i64TicksPerSecond) ||
            ! i64TicksPerSecond)
    {
        //	Don't use Performance Counter
        i64TicksPerSecond	= 0;
    }
    return TRUE;
}
