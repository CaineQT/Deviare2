/*
 * Copyright (C) 2010-2015 Nektra S.A., Buenos Aires, Argentina.
 * All rights reserved. Contact: http://www.nektra.com
 *
 *
 * This file is part of Deviare
 *
 *
 * Commercial License Usage
 * ------------------------
 * Licensees holding valid commercial Deviare licenses may use this file
 * in accordance with the commercial license agreement provided with the
 * Software or, alternatively, in accordance with the terms contained in
 * a written agreement between you and Nektra.  For licensing terms and
 * conditions see http://www.nektra.com/licensing/. Use the contact form
 * at http://www.nektra.com/contact/ for further information.
 *
 *
 * GNU General Public License Usage
 * --------------------------------
 * Alternatively, this file may be used under the terms of the GNU General
 * Public License version 3.0 as published by the Free Software Foundation
 * and appearing in the file LICENSE.GPL included in the packaging of this
 * file.  Please visit http://www.gnu.org/copyleft/gpl.html and review the
 * information to ensure the GNU General Public License version 3.0
 * requirements will be met.
 *
 **/

#include "HookEngineInternals.h"
#include "HookEngineCallDataEntry.h"
#include "HookEngineMiscHelpers.h"
#include "ThreadSuspend.h"
#include <intrin.h>

//-----------------------------------------------------------

#ifdef NKT_ENABLE_MEMORY_TRACKING
  #undef THIS_FILE
  static char THIS_FILE[] = __FILE__;
#endif //NKT_ENABLE_MEMORY_TRACKING

#define CALC_STACK_PRESERVE(x)                             \
                 (NKT_DV_ALIGN_SIZE_T(x) + 4*sizeof(SIZE_T))

#define DUMMY_CALC_STACK_PRESERVE_SIZE      4*sizeof(SIZE_T)

#define MAX_SUSPEND_IPRANGES                              10

//-----------------------------------------------------------

#if defined _M_IX86
  EXTERN_C void __stdcall NktDvSuperHook_x86();
#elif defined _M_X64
  EXTERN_C void __stdcall NktDvSuperHook_x64();
#else
  #error Unsupported platform
#endif

static SIZE_T __stdcall PreCallCommon(__inout CNktDvHookEngine *lpEnginePtr, __in LPVOID lpHookEntry,
                                      __in SIZE_T nStackPointer);
static SIZE_T __stdcall PostCallCommon(__inout CNktDvHookEngine *lpEnginePtr, __in LPVOID lpHookEntry,
                                       __in SIZE_T nStackPointer);

#if defined _M_IX86
static __inline VOID LoadRegisters(__out LPNKT_DV_ASMREGISTERS32 lpRegisters, __out PSIZE_T lpnReturnAddr,
                                   __in SIZE_T nStackPointer, __inout CHookEntry *lpHookEntry, __in BOOL bPreCall);
static __inline VOID SaveRegisters(__in LPNKT_DV_ASMREGISTERS32 lpRegisters, __in SIZE_T nStackPointer);
#elif defined _M_X64
static __inline VOID LoadRegisters(__out LPNKT_DV_ASMREGISTERS64 lpRegisters, __out PSIZE_T lpnReturnAddr,
                                   __in SIZE_T nStackPointer, __inout CHookEntry *lpHookEntry, __in BOOL bPreCall);
static __inline VOID SaveRegisters(__in LPNKT_DV_ASMREGISTERS64 lpRegisters, __in SIZE_T nStackPointer);
#endif
static BOOL SecureMemCopy(__in LPVOID lpDest, __in LPVOID lpSrc, __in SIZE_T nCount);
static BOOL SecureMemIsDiff(__in const void *lpBuf1, __in const void *lpBuf2, __in SIZE_T nCount);
static BOOL SecureCheckStub(__in CHookEntry *lpHookEntry);

//-----------------------------------------------------------

CNktDvHookEngine::CNktDvHookEngine(__in CNktDvHookEngineCallback *_lpCallback) : CNktFastMutex(), CNktMemMgrObj()
{
  NKT_ASSERT(_lpCallback != NULL);
  lpCallback = _lpCallback;
  //----
  if (::QueryPerformanceFrequency(&liTimerFreq) == FALSE)
    liTimerFreq.QuadPart = 0;
  NktInterlockedExchange(&nCallCounter, 0);
  nInjectedCodeMaxSize = 0;
  return;
}

CNktDvHookEngine::~CNktDvHookEngine()
{
  if (bAppIsExiting == FALSE)
  {
    Finalize();
  }
  return;
}

HRESULT CNktDvHookEngine::Initialize()
{
  CNktAutoFastMutex cLock(this);

  Finalize();
  NktInterlockedExchange(&nCallCounter, 0);
  cCustomHandlersMgr.Attach(NKT_MEMMGR_NEW CHookCustomHandlerMgr);
  if (cCustomHandlersMgr == NULL)
  {
    Finalize();
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

VOID CNktDvHookEngine::Finalize()
{
  CNktAutoFastMutex cLock(this);

  UnhookAll();
  cCustomHandlersMgr.Release();
  return;
}

HRESULT CNktDvHookEngine::Hook(__in HOOKINFO aHookInfo[], __in SIZE_T nCount)
{
  CNktDvHookEngineAutoTlsLock cAutoTls;
  CNktAutoFastMutex cLock(this);
  CNktDvThreadSuspend cThreadSuspender;
  CNktDvThreadSuspend::IP_RANGE aIpRanges[MAX_SUSPEND_IPRANGES];
  TNktArrayListWithRelease<CHookEntry*> cNewEntryList;
  TNktLnkLst<CHookEntry>::Iterator it;
  CNktStringW cStrTempW[2];
  CHookEntry *lpHookEntry;
  SIZE_T nHookIdx, nThisRoundSuspCount;
  HRESULT hRes;

  if (aHookInfo == 0)
    return E_POINTER;
  if (nCount == 0)
    return E_INVALIDARG;
  if (FAILED(cAutoTls.GetError()))
    return cAutoTls.GetError();
  if ((::GetVersion() & 0x80000000) != 0)
    return E_NOTIMPL; //reject win9x
  //check parameters
  for (nHookIdx=0; nHookIdx<nCount; nHookIdx++)
  {
    if (aHookInfo[nHookIdx].lpProcToHook == NULL)
      return E_POINTER;
    if ((aHookInfo[nHookIdx].nFlags & (NKT_DV_TMSG_ADDHOOK_FLAG_OnlyPreCall|NKT_DV_TMSG_ADDHOOK_FLAG_OnlyPostCall)) ==
                  (NKT_DV_TMSG_ADDHOOK_FLAG_OnlyPreCall|NKT_DV_TMSG_ADDHOOK_FLAG_OnlyPostCall))
      return E_INVALIDARG;
    //check if already exists
    for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
    {
      if (lpHookEntry->dwId == aHookInfo[nHookIdx].dwHookId)
        return NKT_DVERR_AlreadyExists;
    }
  }
  //create entries for each item
  for (nHookIdx=0; nHookIdx<nCount; nHookIdx++)
  {
    LPBYTE lpInjCodeAddr, lpPtr;
    SIZE_T k, k2, nBaseInjectedCodeSize;
    DWORD dw;

    //allocate new entry
    lpHookEntry = NKT_MEMMGR_NEW CHookEntry;
    if (lpHookEntry != NULL)
    {
      if (cNewEntryList.AddElement(lpHookEntry) == FALSE)
      {
        delete lpHookEntry;
        lpHookEntry = NULL;
      }
    }
    if (lpHookEntry == NULL)
    {
      NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[New Hook Entry]: hRes=%08X",
                        ::GetTickCount(), E_OUTOFMEMORY));
      return E_OUTOFMEMORY;
    }
    lpHookEntry->cCustomHandlersMgr = cCustomHandlersMgr;
    lpHookEntry->dwId = aHookInfo[nHookIdx].dwHookId;
    lpHookEntry->dwSpyMgrId = aHookInfo[nHookIdx].dwSpyMgrHookId;
    lpHookEntry->lpOrigProc = (LPBYTE)(aHookInfo[nHookIdx].lpProcToHook);
    lpHookEntry->lpHookedAddr = HookEng_SkipJumpInstructions((LPBYTE)(aHookInfo[nHookIdx].lpProcToHook));
    lpHookEntry->lpDbFunc = aHookInfo[nHookIdx].lpDbFunc;
    if (aHookInfo[nHookIdx].szFunctionNameW != NULL && aHookInfo[nHookIdx].szFunctionNameW[0] != 0)
    {
      dw = (lpHookEntry->cStrFunctionNameW.Copy(aHookInfo[nHookIdx].szFunctionNameW) != FALSE) ? 1 : 0;
    }
    else if (lpHookEntry->lpDbFunc != NULL)
    {
      dw = (lpHookEntry->cStrFunctionNameW.Copy(lpHookEntry->lpDbFunc->GetName()) != FALSE) ? 1 : 0;
    }
    else
    {
      dw = (lpHookEntry->cStrFunctionNameW.Format(L"0x%0*Ix", (int)(sizeof(SIZE_T)>>3),
                                 (SIZE_T)(aHookInfo[nHookIdx].lpProcToHook)) != FALSE) ? 1 : 0;
    }
    if (dw == 0)
    {
      NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[HookEntry Func Name]: hRes=%08X",
                        ::GetTickCount(), E_OUTOFMEMORY));
      return E_OUTOFMEMORY;
    }
    lpHookEntry->nStackReturnSize = (lpHookEntry->lpDbFunc != NULL) ? 
                    CNktDvFunctionParamsCache::GetStackUsage(lpHookEntry->lpDbFunc) : NKT_SIZE_T_MAX;
    lpHookEntry->nFlags = HOOKENG_FLAG_CallPreCall|HOOKENG_FLAG_CallPostCall;
    if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_AsyncCallbacks) != 0)
      lpHookEntry->nFlags |= HOOKENG_FLAG_AsyncCallbacks;
    if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_DontCallOnLdrLock) != 0)
      lpHookEntry->nFlags |= HOOKENG_FLAG_DontCallOnLdrLock;
    if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_DontCallCustomHandlersOnLdrLock) != 0)
      lpHookEntry->nFlags |= HOOKENG_FLAG_DontCallCustomHandlersOnLdrLock;
    if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_OnlyPreCall) != 0)
      lpHookEntry->nFlags &= ~HOOKENG_FLAG_CallPostCall;
    else if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_OnlyPostCall) != 0)
      lpHookEntry->nFlags &= ~HOOKENG_FLAG_CallPreCall;
    if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_InvalidateCache) != 0)
      lpHookEntry->nFlags |= HOOKENG_FLAG_InvalidateCache;
    if ((aHookInfo[nHookIdx].nFlags & NKT_DV_TMSG_ADDHOOK_FLAG_DisableStackWalk) != 0)
      lpHookEntry->nFlags |= HOOKENG_FLAG_DisableStackWalk;
    hRes = HookEng_FindDll(&(lpHookEntry->hProcDll), aHookInfo[nHookIdx].lpProcToHook);
    if (FAILED(hRes) && hRes != NKT_DVERR_NotFound)
      return hRes;
    //initialize memory reader/writer
    if (cProcMem == NULL)
    {
      cProcMem.Attach(CNktDvProcessMemory::CreateForPID(::GetCurrentProcessId()));
      if (cProcMem == NULL)
      {
        NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[ProcMem assign]: hRes=%08X",
                           ::GetTickCount(), E_OUTOFMEMORY));
        return E_OUTOFMEMORY;
      }
    }
    hRes = lpHookEntry->cFuncParamCache.Initialize(aHookInfo[nHookIdx].lpDbFunc, cProcMem,
                                                   sizeof(SIZE_T)<<3);
    if (FAILED(hRes))
    {
      NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[FuncParam init]: hRes=%08X",
                         ::GetTickCount(), hRes));
      return hRes;
    }
    //attach custom handlers
    if (aHookInfo[nHookIdx].sCustomHandlers.lpData != NULL &&
        aHookInfo[nHookIdx].sCustomHandlers.nDataSize > 0)
    {
      SIZE_T nLen[2], nSize, nCount, nFlags;
      LPBYTE d;

      if ((nSize = aHookInfo[nHookIdx].sCustomHandlers.nDataSize) < sizeof(SIZE_T))
      {
hk_badtransport:
        NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[Transport data]: hRes=%08X",
                           ::GetTickCount(), NKT_DVERR_InvalidTransportData));
        return NKT_DVERR_InvalidTransportData;
      }
      d = aHookInfo[nHookIdx].sCustomHandlers.lpData;
      nCount = (SIZE_T)(*((ULONG NKT_UNALIGNED *)d));
      d += sizeof(ULONG);
      nSize -= sizeof(ULONG);
      while (nCount > 0)
      {
        if (nSize < 3*sizeof(ULONG))
          goto hk_badtransport;
        nFlags = (SIZE_T)(*((ULONG NKT_UNALIGNED *)d));
        d += sizeof(ULONG);
        nLen[0] = (SIZE_T)(*((ULONG NKT_UNALIGNED *)d));
        d += sizeof(ULONG);
        nLen[1] = (SIZE_T)(*((ULONG NKT_UNALIGNED *)d));
        d += sizeof(ULONG);

        nSize -= 3*sizeof(ULONG);
        //copy dll name
        if (nSize < (nLen[0]+nLen[1])*sizeof(WCHAR))
          goto hk_badtransport;
        if (cStrTempW[0].CopyN((LPWSTR)d, nLen[0]) == FALSE ||
            cStrTempW[1].CopyN(((LPWSTR)d)+nLen[0], nLen[1]) == FALSE)
          return E_OUTOFMEMORY;
        nLen[0] *= sizeof(WCHAR);
        nLen[1] *= sizeof(WCHAR);
        d += nLen[0]+nLen[1];
        nSize -= nLen[0]+nLen[1];
        //initialize custom handler
        hRes = lpHookEntry->AttachCustomHandler((LPWSTR)cStrTempW[0], nFlags, (LPWSTR)cStrTempW[1], lpCallback);
        if (FAILED(hRes))
        {
          NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[AttachCustomHandler]: hRes=%08X",
                             ::GetTickCount(), hRes));
          return hRes;
        }
        nCount--;
      }
    }
    //read original stub
    hRes = HookEng_CreateNewStub(lpHookEntry, lpHookEntry->lpHookedAddr, ((aHookInfo[nHookIdx].nFlags &
                                 NKT_DV_TMSG_ADDHOOK_FLAG_DontSkipJumps) == 0) ? TRUE : FALSE);
    if (FAILED(hRes))
    {
      NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[Stub creation]: hRes=%08X",
                         ::GetTickCount(), hRes));
      return hRes;
    }
    //calculate inject code size
#if defined _M_IX86
    lpInjCodeAddr = (LPBYTE)NktDvSuperHook_x86;
#elif defined _M_X64
    lpInjCodeAddr = (LPBYTE)NktDvSuperHook_x64;
#endif
    lpInjCodeAddr = HookEng_SkipJumpInstructions(lpInjCodeAddr);
    for (lpPtr=lpInjCodeAddr; ; lpPtr++)
    {
#if defined _M_IX86
      if (HookEng_ReadUnalignedSizeT(lpPtr) == 0xFFDDFFFF)
        break;
#elif defined _M_X64
      if (HookEng_ReadUnalignedSizeT(lpPtr) == 0xFFDDFFDDFFDDFFFF)
        break;
#endif
    }
    k = (SIZE_T)(lpPtr - lpInjCodeAddr);
    NKT_ASSERT(k > 0);
    nBaseInjectedCodeSize = (k+31) & (~31);
    lpHookEntry->nInjectedCodeSize = nBaseInjectedCodeSize + 2*sizeof(SIZE_T);
    //allocate memory for inject code
    lpHookEntry->lpInjectedCodeAddr = AllocInjectedCode(lpHookEntry->lpHookedAddr);
    if (lpHookEntry->lpInjectedCodeAddr == NULL)
    {
      NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[Code area alloc]: hRes=%08X",
                         ::GetTickCount(), E_OUTOFMEMORY));
      return E_OUTOFMEMORY;
    }
    //copy code
    nktMemCopy(lpHookEntry->lpInjectedCodeAddr, lpInjCodeAddr, nBaseInjectedCodeSize);
    nktMemSet(lpHookEntry->lpInjectedCodeAddr + nBaseInjectedCodeSize, 0, 2*sizeof(SIZE_T));
    for (k=0; k<nBaseInjectedCodeSize; )
    {
      lpPtr = lpHookEntry->lpInjectedCodeAddr + k;
#if defined _M_IX86
      switch (HookEng_ReadUnalignedSizeT(lpPtr))
      {
        case 0xFFDDFF01: //USAGE_COUNT
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)(lpHookEntry->lpInjectedCodeAddr) +
                                             nBaseInjectedCodeSize + sizeof(SIZE_T));
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF02: //UNINSTALL_DISABLE_FLAGS
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)(lpHookEntry->lpInjectedCodeAddr) +
                                             nBaseInjectedCodeSize);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF03: //HOOK_ENGINE_PTR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)this);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF04: //HOOK_ENTRY_PTR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)lpHookEntry);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF05: //PRECALL_ADDR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)PreCallCommon);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF06: //POSTCALL_ADDR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)PostCallCommon);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF07: //ORIGINAL_STUB_AND_JUMP
          nktMemCopy(lpPtr, lpHookEntry->aNewStub, lpHookEntry->nNewStubSize);
          lpPtr += lpHookEntry->nNewStubSize;
          lpPtr[0] = 0xE9; //JMP NEAR relative
          k2 = (SIZE_T)(lpHookEntry->lpHookedAddr) + lpHookEntry->nOriginalStubSize - (SIZE_T)lpPtr - 5;
          HookEng_WriteUnalignedSizeT(lpPtr+1, k2);
          k += 64 + (1+4);
          break;
        case 0xFFDDFF08: //CONTINUE_AFTER_CALL_MARK
          lpHookEntry->lpAfterCallMark = lpPtr + sizeof(SIZE_T);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF09: //AFTER_CALL_STACK_PRESERVE_SIZE
          //preserve stack size for postcall plus some extra
          if (lpHookEntry->nStackReturnSize == NKT_SIZE_T_MAX)
            k2 = CALC_STACK_PRESERVE(DUMMY_CALC_STACK_PRESERVE_SIZE);
          else
            k2 = CALC_STACK_PRESERVE(lpHookEntry->nStackReturnSize);
          HookEng_WriteUnalignedSizeT(lpPtr, k2);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFF10: //AFTER_CALL_STACK_PRESERVE_SIZE2
          //preserve stack size for postcall plus some extra
          if (lpHookEntry->nStackReturnSize == NKT_SIZE_T_MAX)
            k2 = CALC_STACK_PRESERVE(DUMMY_CALC_STACK_PRESERVE_SIZE);
          else
            k2 = CALC_STACK_PRESERVE(lpHookEntry->nStackReturnSize);
          HookEng_WriteUnalignedSizeT(lpPtr, 0); //zero
          lpPtr[0] = (BYTE)( k2       & 0xFF);
          lpPtr[1] = (BYTE)((k2 >> 8) & 0xFF);
          k += sizeof(SIZE_T);
          break;
        default:
          k++;
          break;
      }
#elif defined _M_X64
      switch (HookEng_ReadUnalignedSizeT(lpPtr))
      {
        case 0xFFDDFFDDFFDDFF01: //USAGE_COUNT
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)(lpHookEntry->lpInjectedCodeAddr) +
                                             nBaseInjectedCodeSize + sizeof(SIZE_T));
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF02: //UNINSTALL_DISABLE_FLAGS
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)(lpHookEntry->lpInjectedCodeAddr) +
                                             nBaseInjectedCodeSize);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF03: //HOOK_ENGINE_PTR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)this);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF04: //HOOK_ENTRY_PTR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)lpHookEntry);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF05: //PRECALL_ADDR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)PreCallCommon);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF06: //POSTCALL_ADDR
          HookEng_WriteUnalignedSizeT(lpPtr, (SIZE_T)PostCallCommon);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF07: //ORIGINAL_STUB_AND_JUMP
          nktMemCopy(lpPtr, lpHookEntry->aNewStub, lpHookEntry->nNewStubSize);
          lpPtr += lpHookEntry->nNewStubSize;
          lpPtr[0] = 0xFF;  lpPtr[1] = 0x25; //JMP QWORD PTR [RIP+0h]
          HookEng_WriteUnalignedULong(lpPtr+2, 0);
          k2 = (SIZE_T)(lpHookEntry->lpHookedAddr) + lpHookEntry->nOriginalStubSize;
          HookEng_WriteUnalignedSizeT(lpPtr+6, k2);
          k += 64 + (6+8);
          break;
        case 0xFFDDFFDDFFDDFF08: //CONTINUE_AFTER_CALL_MARK
          lpHookEntry->lpAfterCallMark = lpPtr + sizeof(SIZE_T);
          k += sizeof(SIZE_T);
          break;
        case 0xFFDDFFDDFFDDFF09: //AFTER_CALL_STACK_PRESERVE_SIZE
          //preserve stack size for postcall plus some extra
          if (lpHookEntry->nStackReturnSize == NKT_SIZE_T_MAX)
            k2 = CALC_STACK_PRESERVE(DUMMY_CALC_STACK_PRESERVE_SIZE);
          else
            k2 = CALC_STACK_PRESERVE(lpHookEntry->nStackReturnSize);
          HookEng_WriteUnalignedSizeT(lpPtr, k2);
          k += sizeof(SIZE_T);
          break;
        default:
          k++;
          break;
      }
#endif
    }
    lpHookEntry->lpUninstalledDisabledAddr = lpHookEntry->lpInjectedCodeAddr + nBaseInjectedCodeSize;
    lpHookEntry->lpUninstalledDisabledAddr[0] = 1; //hook is initially disabled
    lpHookEntry->lpUsageCounterAddr = (PSIZE_T)(lpHookEntry->lpInjectedCodeAddr + nBaseInjectedCodeSize +
                                              sizeof(SIZE_T));
    //replace original proc with a jump
    dw = (DWORD)((SIZE_T)(lpHookEntry->lpInjectedCodeAddr) - (SIZE_T)(lpHookEntry->lpHookedAddr)) - 5;
    nktMemCopy(lpHookEntry->aModifiedStub, lpHookEntry->aOriginalStub, 8);
    lpHookEntry->aModifiedStub[0] = 0xE9; //JMP
    lpHookEntry->aModifiedStub[1] = (BYTE)( dw        & 0xFF);
    lpHookEntry->aModifiedStub[2] = (BYTE)((dw >>  8) & 0xFF);
    lpHookEntry->aModifiedStub[3] = (BYTE)((dw >> 16) & 0xFF);
    lpHookEntry->aModifiedStub[4] = (BYTE)((dw >> 24) & 0xFF);
  }
  //hook new items
  for (nHookIdx=nThisRoundSuspCount=0; nHookIdx<nCount; )
  {
    SIZE_T k, k2;
    DWORD dw[64];

    if (nThisRoundSuspCount == 0)
    {
      //suspend threads
      nThisRoundSuspCount = (nCount-nHookIdx > MAX_SUSPEND_IPRANGES) ? MAX_SUSPEND_IPRANGES :
                                                                       (nCount-nHookIdx);
      for (k=0; k<nThisRoundSuspCount; k++)
      {
        aIpRanges[k].nStart = (SIZE_T)(cNewEntryList[nHookIdx+k]->lpHookedAddr);
        aIpRanges[k].nEnd = aIpRanges[k].nStart + HOOKENG_JUMP_TO_HOOK_SIZE;
      }
      hRes = cThreadSuspender.SuspendAll(aIpRanges, nThisRoundSuspCount);
      if (FAILED(hRes))
      {
        NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[Suspend threads]: hRes=%08X",
                           ::GetTickCount(), hRes));
err_uninstall_and_destroy:
        for (nHookIdx=k2=0; nHookIdx<nCount; nHookIdx++)
        {
          if (cNewEntryList[nHookIdx]->nInstalledCode != 0)
          {
            dw[k2++] = cNewEntryList[nHookIdx]->dwId;
            if (k2 >= NKT_DV_ARRAYLEN(dw))
            {
              Unhook(dw, k2);
              k2 = 0;
            }
          }
        }
        if (k2 > 0)
          Unhook(dw, k2);
        return hRes;
      }
    }
    for (k=0; k<nThisRoundSuspCount; k++)
    {
      //replace each entry point
      dw[0] = 0;
      hRes = nktDvDynApis_NtProtectVirtualMemory(cNewEntryList[nHookIdx+k]->lpHookedAddr, HOOKENG_JUMP_TO_HOOK_SIZE,
                                                 PAGE_EXECUTE_READWRITE, &dw[0]);
      if (FAILED(hRes))
      {
        hRes = nktDvDynApis_NtProtectVirtualMemory(cNewEntryList[nHookIdx+k]->lpHookedAddr, HOOKENG_JUMP_TO_HOOK_SIZE,
                                                   PAGE_EXECUTE_WRITECOPY, &dw[0]);
      }
      if (FAILED(hRes))
      {
        cThreadSuspender.ResumeAll();
        NKT_DEBUGPRINTLNA(Nektra::dlHookEngine, ("%lu) CNktDvHookEngine[VirtualProtect]: hRes=%08X",
                           ::GetTickCount(), hRes));
        goto err_uninstall_and_destroy;
      }
      nktMemCopy(cNewEntryList[nHookIdx+k]->lpHookedAddr, cNewEntryList[nHookIdx+k]->aModifiedStub,
                 HOOKENG_JUMP_TO_HOOK_SIZE);
      nktDvDynApis_NtProtectVirtualMemory(cNewEntryList[nHookIdx+k]->lpHookedAddr, HOOKENG_JUMP_TO_HOOK_SIZE, dw[0],
                                          &dw[0]);
      //flush instruction cache
      nktDvDynApis_NtFlushInstructionCache(cNewEntryList[nHookIdx+k]->lpHookedAddr, 32);
      //mark as installed
      NktInterlockedExchange(&(cNewEntryList[nHookIdx+k]->nInstalledCode), 1);
      cHooksList.PushTail((CHookEntry*)cNewEntryList[nHookIdx+k]);
      ((CHookEntry*)cNewEntryList[nHookIdx+k])->AddRef();
    }
    //advance count
    nHookIdx += nThisRoundSuspCount;
    //check if we can proceed with the next hook with this
    nThisRoundSuspCount = 0;
    for (k=nHookIdx; k<nCount; k++) {
      k2 = (SIZE_T)(cNewEntryList[k]->lpHookedAddr);
      if (cThreadSuspender.CheckIfThreadIsInRange(k2, k2+HOOKENG_JUMP_TO_HOOK_SIZE) == FALSE)
        break;
      nThisRoundSuspCount++;
    }
    if (nThisRoundSuspCount == 0)
    {
      //resume threads
      cThreadSuspender.ResumeAll();
    }
  }
  return S_OK;
}

HRESULT CNktDvHookEngine::Unhook(__in LPDWORD lpdwHookIds, __in SIZE_T nCount)
{
  CNktDvHookEngineAutoTlsLock cAutoTls;
  CNktAutoFastMutex cLock(this);
  TNktLnkLst<CHookEntry>::Iterator it;
  CHookEntry *lpHookEntry;
  CNktDvThreadSuspend cThreadSuspender;
  CNktDvThreadSuspend::IP_RANGE aIpRanges[2];
  DWORD dw;
  BOOL b, bInUse, bFreeInjectedCode;
  SIZE_T k, nHookIdx, nIpRangesCount;

  //NOTE: should this be moved to a background thread???
  if (FAILED(cAutoTls.GetError()))
    return cAutoTls.GetError();
  if (lpdwHookIds == NULL)
    return E_POINTER;
  if (nCount == 0)
    return E_INVALIDARG;
  //check parameters
  for (nHookIdx=nIpRangesCount=0; nHookIdx<nCount; nHookIdx++)
  {
    for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
    {
      if (lpHookEntry->dwId == lpdwHookIds[nHookIdx])
        break;
    }
    if (lpHookEntry == NULL)
      continue; //hook not found
    //mark the hook as uninstalled
    NktInterlockedExchange(&(lpHookEntry->nInstalledCode), 2);
    _ReadWriteBarrier();
    lpHookEntry->lpUninstalledDisabledAddr[1] = 1;
    _ReadWriteBarrier();
    //set-up ranges
    aIpRanges[0].nStart = (SIZE_T)(lpHookEntry->lpHookedAddr);
    aIpRanges[0].nEnd = aIpRanges[0].nStart + lpHookEntry->nOriginalStubSize;
    aIpRanges[1].nStart = (SIZE_T)(lpHookEntry->lpInjectedCodeAddr);
    aIpRanges[1].nEnd = aIpRanges[1].nStart + lpHookEntry->nInjectedCodeSize;
    if (nIpRangesCount > 0)
    {
      //check if a previous thread suspension can be used for the current unhook item
      if (cThreadSuspender.CheckIfThreadIsInRange(aIpRanges[0].nStart, aIpRanges[0].nEnd) != FALSE ||
          cThreadSuspender.CheckIfThreadIsInRange(aIpRanges[1].nStart, aIpRanges[1].nEnd) != FALSE)
      {
        nIpRangesCount = 0;
        cThreadSuspender.ResumeAll(); //resume last
      }
    }
    //suspend threads
    bFreeInjectedCode = FALSE;
    if (nIpRangesCount == 0)
    {
      //we have to suspend the threads
      nIpRangesCount = 2;
      bInUse = TRUE;
      for (k=20; k>0; k--)
      {
        if (FAILED(cThreadSuspender.SuspendAll(aIpRanges, nIpRangesCount)))
          break;
        //check if still in use
        if (*(lpHookEntry->lpUsageCounterAddr) == 0)
        {
          bInUse = FALSE;
          break;
        }
        //the hook is in use, so wait a moment
        cThreadSuspender.ResumeAll();
        if (k > 1)
          ::Sleep(10);
      }
    }
    else
    {
      //iujuuuu!!! we can reuse the previous suspension
      bInUse = (*(lpHookEntry->lpUsageCounterAddr) == 0) ? FALSE : TRUE;
    }
    if (bInUse == FALSE)
    {
      //check if the modified stub is the same than the current one
      b = SecureCheckStub(lpHookEntry);
      if (b != FALSE)
      {
        //try to restore original code at entry point
        dw = 0;
        b = SUCCEEDED(nktDvDynApis_NtProtectVirtualMemory(lpHookEntry->lpHookedAddr, HOOKENG_JUMP_TO_HOOK_SIZE,
                                                          PAGE_EXECUTE_READWRITE, &dw));
        if (b == FALSE)
        {
          b = SUCCEEDED(nktDvDynApis_NtProtectVirtualMemory(lpHookEntry->lpHookedAddr, HOOKENG_JUMP_TO_HOOK_SIZE,
                                                            PAGE_EXECUTE_WRITECOPY, &dw));
        }
        if (b != FALSE)
        {
          b = SecureMemCopy(lpHookEntry->lpHookedAddr, lpHookEntry->aOriginalStub, HOOKENG_JUMP_TO_HOOK_SIZE);
          ::nktDvDynApis_NtProtectVirtualMemory(lpHookEntry->lpHookedAddr, HOOKENG_JUMP_TO_HOOK_SIZE, dw, &dw);
          //flush instruction cache
          nktDvDynApis_NtFlushInstructionCache(lpHookEntry->lpHookedAddr, 32);
        }
        if (b != FALSE)
        {
          //at this point, the original entry point code was restored so i can release virtual
          //memory blocks release trampoline
          bFreeInjectedCode = TRUE;
        }
      }
    }
    else
    {
      //do not remove a hook in-use
      //NOTE: This will produce a leak although it should occur almost never, sorry
      NktInterlockedExchange(&(lpHookEntry->nInstalledCode), 3);
    }
    //free hook entry but do not free hooking code and libraries if physical unhook
    //did not succeeded
    if (bFreeInjectedCode == FALSE)
    {
      lpHookEntry->hProcDll = NULL;
      lpHookEntry->lpInjectedCodeAddr = NULL;
    }
  }
  //resume all if threads were suspended
  cThreadSuspender.ResumeAll();
  //detach unhooked items from the chain
  for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
  {
    switch (lpHookEntry->nInstalledCode)
    {
      case 2:
        cHooksList.Remove(lpHookEntry);
        if (bAppIsExiting == FALSE)
          FreeInjectedCode(lpHookEntry);
        lpHookEntry->Release();
        break;
      case 3:
        cHooksList.Remove(lpHookEntry);
        lpHookEntry->DetachAllCustomHandlers();
        break;
    }
  }
  return S_OK;
}

VOID CNktDvHookEngine::DllUnloadUnhook(__in HINSTANCE hDll)
{
  CNktAutoFastMutex cLock(this);
  TNktLnkLst<CHookEntry>::IteratorRev it;
  CHookEntry *lpHookEntry;
  DWORD dw[64];
  SIZE_T k;
  BOOL bContinue;

  //mark hooks as uninstalled first
  for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
  {
    if (lpHookEntry->hProcDll == hDll)
    {
      _ReadWriteBarrier();
      lpHookEntry->lpUninstalledDisabledAddr[1] = 1;
      _ReadWriteBarrier();
    }
  }
  //unhook in reverse order
  for (bContinue=TRUE; bContinue!=FALSE; )
  {
    bContinue = FALSE;
    k = 0;
    for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
    {
      if (lpHookEntry->hProcDll == hDll)
      {
        bContinue = TRUE;
        dw[k++] = lpHookEntry->dwId;
        if (k >= NKT_DV_ARRAYLEN(dw))
        {
          Unhook(dw, k);
          k = 0;
        }
      }
    }
    if (k > 0)
      Unhook(dw, k);
  }
  return;
}

VOID CNktDvHookEngine::UnhookAll()
{
  CNktAutoFastMutex cLock(this);
  TNktLnkLst<CHookEntry>::IteratorRev itRev;
  DWORD dw[64];
  SIZE_T k;
  CHookEntry *lpHookEntry;

  //mark all hooks as uninstalled first
  for (lpHookEntry=itRev.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=itRev.Next())
  {
    _ReadWriteBarrier();
    lpHookEntry->lpUninstalledDisabledAddr[1] = 1;
    _ReadWriteBarrier();
  }
  //unhook in reverse order
  while (cHooksList.IsEmpty() == FALSE)
  {
    for (lpHookEntry=itRev.Begin(cHooksList),k=0; lpHookEntry!=NULL && k<NKT_DV_ARRAYLEN(dw);
         lpHookEntry=itRev.Next())
    {
      dw[k++] = lpHookEntry->dwId;
    }
    if (k > 0)
      Unhook(dw, k);
  }
  return;
}

HRESULT CNktDvHookEngine::EnableHook(__in DWORD dwHookId, __in BOOL bEnable)
{
  CNktAutoFastMutex cLock(this);
  TNktLnkLst<CHookEntry>::Iterator it;
  CHookEntry *lpHookEntry;

  for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
  {
    if (lpHookEntry->dwId == dwHookId)
    {
      //write enable/disable flag
      _ReadWriteBarrier();
      lpHookEntry->lpUninstalledDisabledAddr[0] = (bEnable != FALSE) ? 0 : 1;
      _ReadWriteBarrier();
      //done
      return S_OK;
    }
  }
  return NKT_DVERR_NotFound;
}

BOOL CNktDvHookEngine::CheckIfInTrampoline(__in SIZE_T nCurrIP)
{
  CNktAutoFastMutex cLock(this);
  TNktLnkLst<CHookEntry>::Iterator it;
  CHookEntry *lpHookEntry;

  for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
  {
    if (nCurrIP >= (SIZE_T)(lpHookEntry->lpInjectedCodeAddr) &&
        nCurrIP < (SIZE_T)(lpHookEntry->lpInjectedCodeAddr)+lpHookEntry->nInjectedCodeSize)
      return TRUE;
  }
  return FALSE;
}

HRESULT CNktDvHookEngine::CheckOverwrittenHooks()
{
  TNktLnkLst<CHookEntry>::Iterator it;
  CHookEntry *lpHookEntry;
  DWORD aHookIdsList[1024];
  SIZE_T nCount; //,k;
  HRESULT hRes;

  hRes = S_OK;
  nCount = 0;
  {
    CNktAutoFastMutex cLock(this);

    for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL && nCount<NKT_DV_ARRAYLEN(aHookIdsList);
         lpHookEntry=it.Next())
    {
      if (lpHookEntry->bChangedInformed == FALSE && (lpHookEntry->dwId & 0x80000000) == 0)
      {
        if (SecureMemIsDiff(lpHookEntry->lpHookedAddr, lpHookEntry->aModifiedStub, 5) != FALSE)
        {
          lpHookEntry->bChangedInformed = TRUE;
          aHookIdsList[nCount++] = lpHookEntry->dwId;
        }
      }
    }
  }
  if (nCount > 0)
  {
    hRes = lpCallback->HEC_OnHookOverwritten(aHookIdsList, nCount);
  }
  //TODO: We should retry check if too many hooks were modified but if I lock the list again I can
  //      generate a deadlock, mainly in ThinApp applications
  return hRes;
}

HRESULT CNktDvHookEngine::QueryOverwrittenHooks(__in SIZE_T nCount, __in LPDWORD lpdwHookIds, __out LPBYTE lpnResults)
{
  CNktAutoFastMutex cLock(this);
  TNktLnkLst<CHookEntry>::Iterator it;
  CHookEntry *lpHookEntry;
  SIZE_T i;

  nktMemSet(lpnResults, 0, nCount);
  for (i=0; i<nCount; i++)
  {
    if (lpdwHookIds[i] > 0 && (lpdwHookIds[i] & 0x80000000) == 0)
    {
      for (lpHookEntry=it.Begin(cHooksList); lpHookEntry!=NULL; lpHookEntry=it.Next())
      {
        if (lpdwHookIds[i] == lpHookEntry->dwId)
        {
          if (SecureMemIsDiff(lpHookEntry->lpHookedAddr, lpHookEntry->aModifiedStub, 5) != FALSE)
            lpnResults[i] = 1;
          break;
        }
      }
    }
  }
  return S_OK;
}

LONG CNktDvHookEngine::GetNextCallCookie()
{
  LONG nCookie;

  do
  {
    nCookie = NktInterlockedIncrement(&nCallCounter);
  }
  while (nCookie == 0);
  return nCookie;
}

SIZE_T CNktDvHookEngine::PreCall(__in LPVOID lpHookEntryVoid, __inout CNktDvTlsData *lpTlsData,
                                 __in SIZE_T nStackPointer, __inout DWORD &dwOsLastError)
{
  CHookEntry *lpHookEntry = (CHookEntry*)lpHookEntryVoid;
  CHookEngCallDataEntry *lpCallDataEntry, *lpPrevCallDataEntry;
  CNktDvHookEngine::CALLINFO sCallInfo;
  CHookEngCallDataEntry::CTiming cFunctionTimingTemp;
  TNktComPtr<CNktDvFunctionParamsCache::FUNCTION_PARAMS> cFuncParams;
  TNktComPtr<CNktDvParamsEnumerator> cCustomParams;
  SIZE_T nReturnAddr, nRetCode;
  BOOL bPreCallCalled;
  HRESULT hRes;

  if (lpHookEntryVoid == NULL)
  {
    lpCallback->HEC_OnError(E_OUTOFMEMORY);
    return 0;
  }
  //get function timing (1a)
  cFunctionTimingTemp.Initialize(liTimerFreq);
  lpTlsData->ThreadTimesAccumulate(cFunctionTimingTemp.ftCreationTime, cFunctionTimingTemp.nKernelMs,
                                   cFunctionTimingTemp.nUserMs, cFunctionTimingTemp.nCpuClockCycles);
  //debug print info
  NKT_DEBUGPRINTLNA(Nektra::dlHookEnginePreCall, ("%lu) HookEngine[PreCall]: Entry=%IXh, OrigProc=%IXh, "
                    "InjCode=%IXh (%S)", ::GetTickCount(), lpHookEntry, lpHookEntry->lpHookedAddr,
                    lpHookEntry->lpInjectedCodeAddr, (lpHookEntry->lpDbFunc != NULL) ?
                    lpHookEntry->lpDbFunc->GetName() : L""));
  //check for system thread
  if (lpCallback->HEC_IsSystemThread() != FALSE)
  {
    nRetCode = 0; //ignore this call
    goto pc_exit;
  }
  //invalidate cache?
  if ((lpHookEntry->nFlags & HOOKENG_FLAG_InvalidateCache) != 0)
    CNktDvProcess::MarkModulesEnumeratorAsDirty(::GetCurrentProcessId(), NULL);
  //get function param item from cache
  cFuncParams.Attach(lpHookEntry->cFuncParamCache.GetFuncParam());
  if (cFuncParams == NULL)
  {
    lpCallback->HEC_OnError(E_OUTOFMEMORY);
    nRetCode = 0; //ignore this call
    goto pc_exit;
  }
  //get a calldata object from the TLS free list or allocate one
  lpCallDataEntry = (CHookEngCallDataEntry*)(lpTlsData->sHookCallData.cFreeList.PopHead());
  if (lpCallDataEntry == NULL)
  {
    lpCallDataEntry = NKT_MEMMGR_NEW CHookEngCallDataEntry;
    if (lpCallDataEntry == NULL)
    {
      lpCallback->HEC_OnError(E_OUTOFMEMORY);
      nRetCode = 0; //ignore this call
      goto pc_exit;
    }
  }
  nktMemSet(lpCallDataEntry->aIntercallCustomData, 0, NKT_DV_ARRAYLEN(lpCallDataEntry->aIntercallCustomData));
  //obtain registers data
  LoadRegisters(&(lpCallDataEntry->sCallState.sAsmRegisters), &nReturnAddr, nStackPointer, lpHookEntry, TRUE);
  //assign addresses to parameters
  hRes = CNktDvFunctionParamsCache::PreCall_AssignAddresses(cFuncParams, lpHookEntry->lpDbFunc,
                                                            &(lpCallDataEntry->sCallState.sAsmRegisters));
  if (FAILED(hRes))
  {
    lpCallDataEntry->RemoveNode();
    lpCallDataEntry->cFuncParams.Release();
    lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
    //inform about the error
    lpCallback->HEC_OnError(hRes);
    nRetCode = 0; //ignore this call
    goto pc_exit;
  }
  //init last error and other stuff
  lpCallDataEntry->lpHookEntry = lpHookEntry;
  while ((lpCallDataEntry->nCallDataItemId = NktInterlockedIncrement(&(lpHookEntry->nCallCounter))) == 0);
  lpCallDataEntry->sCallState.dwOsLastError = dwOsLastError;
  lpCallDataEntry->sCallState.dwSkipCall = 0;
  lpPrevCallDataEntry = (CHookEngCallDataEntry*)(lpTlsData->sHookCallData.cInUseStack.GetTail());
  lpCallDataEntry->dwChainDepth = (lpPrevCallDataEntry != NULL) ? (lpPrevCallDataEntry->dwChainDepth+1) : 1;
  lpTlsData->sHookCallData.cInUseStack.PushTail(lpCallDataEntry);
  //save return address
  lpCallDataEntry->nReturnAddr = nReturnAddr;
  lpCallDataEntry->nAfterCallAddr = (SIZE_T)(lpHookEntry->lpAfterCallMark);
  //attach function parameters
  lpCallDataEntry->cFuncParams = cFuncParams;
  while ((lpCallDataEntry->nCookie = NktInterlockedIncrement(&(nCallCounter))) == 0);
  //get stack info
  nktMemSet(lpCallDataEntry->sCallState.nStackTrace, 0, sizeof(lpCallDataEntry->sCallState.nStackTrace));
  if ((lpHookEntry->nFlags & HOOKENG_FLAG_DisableStackWalk) == 0)
  {
#if defined _M_IX86
    GetStackTrace(lpCallDataEntry->sCallState.nStackTrace, NKT_DV_TMSG_ONHOOKCALLED_StackTraceDepth, 0,
                  lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Esp,
                  lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Ebp, 0, 0, lpTlsData);
#elif defined _M_X64
    GetStackTrace(lpCallDataEntry->sCallState.nStackTrace, NKT_DV_TMSG_ONHOOKCALLED_StackTraceDepth, 0,
                  lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Rip,
                  lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Rsp, 0, 0, lpTlsData);
#endif
  }
  //init call info data
  sCallInfo.dwHookId = lpHookEntry->dwId;
  sCallInfo.nCookie = lpCallDataEntry->nCookie;
  sCallInfo.nPhase = CNktDvHookEngine::phPreCall;
  sCallInfo.lpDbFunc = lpHookEntry->lpDbFunc;
  sCallInfo.bAsyncCallbacks = ((lpHookEntry->nFlags & HOOKENG_FLAG_AsyncCallbacks) != 0) ? TRUE : FALSE;
  sCallInfo.lpCallState = &(lpCallDataEntry->sCallState);
  sCallInfo.dwThreadId = ::GetCurrentThreadId();
  sCallInfo.nCurrentTimeMs = cFunctionTimingTemp.nTimeMs;
  sCallInfo.nElapsedTimeMs = sCallInfo.nChildsElapsedTimeMs = 0.0;
  sCallInfo.nKernelTimeMs = lpTlsData->sThreadTimes.nKernelTimeMs;
  sCallInfo.nUserTimeMs = lpTlsData->sThreadTimes.nUserTimeMs;
  sCallInfo.nCpuClockCycles = lpTlsData->sThreadTimes.nCpuClockCycles;
  sCallInfo.dwChainDepth = lpCallDataEntry->dwChainDepth;
  //call the callback
  bPreCallCalled = FALSE;
  hRes = S_OK; //assume all is ok if not doing a precall
  if ((lpHookEntry->nFlags & HOOKENG_FLAG_CallPreCall) != 0 &&
      ((lpHookEntry->nFlags & HOOKENG_FLAG_DontCallOnLdrLock) == 0 ||
       nktDvNtIsLoaderLoaderLockHeld(FALSE) == FALSE))
  {
    //but first call the custom handlers if they were provided
    if ((lpHookEntry->nFlags & HOOKENG_FLAG_DontCallCustomHandlersOnLdrLock) == 0 ||
        nktDvNtIsLoaderLoaderLockHeld(FALSE) == FALSE)
    {
      hRes = lpHookEntry->ProcessCustomHandler(&cCustomParams, &sCallInfo, cFuncParams->cParameters,
                                               cFuncParams->cResult, lpCallDataEntry->aIntercallCustomData, lpCallback);
    }
    //call the callback
    if (hRes == S_FALSE)
    {
      //a custom handler inform us not to send the message to spymgr
      hRes = S_OK;
    }
    else if (SUCCEEDED(hRes))
    {
      hRes = lpCallback->HEC_OnHookCalled(sCallInfo, lpCallDataEntry->aIntercallCustomData,
                                          lpCallDataEntry->cFuncParams, cCustomParams);
      bPreCallCalled = TRUE;
    }
  }
  //re-assign addresses of parameters after reporting to spymgr and save original stack values
  nktMemCopy(&(lpCallDataEntry->sCallState.sPreCallAsmRegisters),
             &(lpCallDataEntry->sCallState.sAsmRegisters),
             sizeof(lpCallDataEntry->sCallState.sAsmRegisters));
  CNktDvFunctionParamsCache::PreCall_ReassignAddressesAndSaveOriginalStackValues(cFuncParams);
  if (hRes != S_OK)
  {
    //check against S_OK
    //remove calldata from in-use list and move to the free list
    lpCallDataEntry->RemoveNode();
    lpCallDataEntry->cFuncParams.Release();
    lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
    //inform error (if not S_FALSE)
    if (FAILED(hRes))
      lpCallback->HEC_OnError(hRes);
    //set thread times
    cFunctionTimingTemp.Initialize(liTimerFreq);
    lpTlsData->ThreadTimesSetLast(cFunctionTimingTemp.nKernelMs, cFunctionTimingTemp.nUserMs,
                                  cFunctionTimingTemp.nCpuClockCycles);
    //ignore this call
    nRetCode = 0;
    goto pc_exit;
  }
  //check skip call
  if (sCallInfo.lpCallState->dwSkipCall != 0 && lpHookEntry->nStackReturnSize != NKT_SIZE_T_MAX)
  {
    nRetCode = 0x80000000 | lpHookEntry->nStackReturnSize;
    //remove calldata from in-use list and move to the free list
    lpCallDataEntry->RemoveNode();
    lpCallDataEntry->cFuncParams.Release();
    lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
    //update registers
    SaveRegisters(&(sCallInfo.lpCallState->sAsmRegisters), nStackPointer);
    dwOsLastError = sCallInfo.lpCallState->dwOsLastError;
    //set thread times
    cFunctionTimingTemp.Initialize(liTimerFreq);
    lpTlsData->ThreadTimesSetLast(cFunctionTimingTemp.nKernelMs, cFunctionTimingTemp.nUserMs,
                                  cFunctionTimingTemp.nCpuClockCycles);
    //skip call
    goto pc_exit;
  }
  //get function timing (1b)
  lpCallDataEntry->nCurrentTimeMs = sCallInfo.nCurrentTimeMs;
  lpCallDataEntry->cFunctionTiming[0] = cFunctionTimingTemp;
  lpCallDataEntry->cFunctionTiming[1].Initialize(liTimerFreq);
  lpCallDataEntry->nChildsElapsedTimeMs = lpCallDataEntry->nChildOverHeadMs = 0.0;
  lpTlsData->ThreadTimesSetLast(lpCallDataEntry->cFunctionTiming[1].nKernelMs,
                                lpCallDataEntry->cFunctionTiming[1].nUserMs,
                                lpCallDataEntry->cFunctionTiming[1].nCpuClockCycles);
  if (bPreCallCalled != FALSE)
  {
    //update registers
    SaveRegisters(&(lpCallDataEntry->sCallState.sAsmRegisters), nStackPointer);
    dwOsLastError = lpCallDataEntry->sCallState.dwOsLastError;
  }
  //replace return address
#if defined _M_IX86
  *((LPVOID*)(sCallInfo.lpCallState->sAsmRegisters.sInteger.Esp)) = lpHookEntry->lpAfterCallMark;
#elif defined _M_X64
  *((LPVOID*)(sCallInfo.lpCallState->sAsmRegisters.sInteger.Rsp)) = lpHookEntry->lpAfterCallMark;
#endif
  nRetCode = 1; //continue
pc_exit:
  NKT_DEBUGPRINTLNA(Nektra::dlHookEnginePreCall, ("%lu) HookEngine[PreCall-Exit]: Entry=%IXh, Ret=%08X",
                    ::GetTickCount(), lpHookEntry, (DWORD)nRetCode));
  return nRetCode;
}

SIZE_T CNktDvHookEngine::PostCall(__in LPVOID lpHookEntryVoid, __inout CNktDvTlsData *lpTlsData,
                                  __in SIZE_T nStackPointer, __inout DWORD &dwOsLastError)
{
  CHookEntry *lpHookEntry = (CHookEntry*)lpHookEntryVoid;
  CHookEngCallDataEntry *lpCallDataEntry, *lpParentCallDataEntry;
  TNktComPtr<CNktDvParamsEnumerator> cCustomParams;
  CALLINFO sCallInfo;
  SIZE_T nRetAddr;
  CHookEngCallDataEntry::CTiming cFunctionTimingTemp[2];
#if defined _M_IX86
  NKT_DV_ASMREGISTERS32 sAsmRegisters;
#elif defined _M_X64
  NKT_DV_ASMREGISTERS64 sAsmRegisters;
#endif
  double nTimeDiffMs;
  HRESULT hRes;
  BOOL bPostCallCalled;

  //get function timing (2a)
  cFunctionTimingTemp[0].Initialize(liTimerFreq);
  lpTlsData->ThreadTimesAccumulate(cFunctionTimingTemp[0].ftCreationTime, cFunctionTimingTemp[0].nKernelMs,
                                   cFunctionTimingTemp[0].nUserMs, cFunctionTimingTemp[0].nCpuClockCycles);
  //debug print info
  NKT_DEBUGPRINTLNA(Nektra::dlHookEnginePostCall, ("%lu) HookEngine[PostCall]: Entry=%IXh, "
                    "OrigProc=%IXh, InjCode=%IXh (%S)", ::GetTickCount(), lpHookEntry,
                    lpHookEntry->lpHookedAddr, lpHookEntry->lpInjectedCodeAddr,
                    (lpHookEntry->lpDbFunc != NULL) ? lpHookEntry->lpDbFunc->GetName() : L""));
  //obtain registers data
  LoadRegisters(&sAsmRegisters, NULL, nStackPointer, lpHookEntry, FALSE);
  //find the original return address (iterate the LIFO queue because the some item may be skipped by an exception)
  while (1)
  {
    lpCallDataEntry = (CHookEngCallDataEntry*)(lpTlsData->sHookCallData.cInUseStack.GetTail());
    if (lpCallDataEntry == NULL || lpCallDataEntry->lpHookEntry == lpHookEntry)
      break;
    //remove calldata from in-use list and move to the free list
    lpCallDataEntry->RemoveNode();
    lpCallDataEntry->cFuncParams.Release();
    lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
  }
  NKT_ASSERT(lpCallDataEntry != NULL);
  if (lpCallDataEntry == NULL)
  {
    ::MessageBoxW(NULL, L"Invalid internal state (1). Terminating application...", L"Deviare2", MB_OK|MB_ICONASTERISK);
    ::TerminateProcess(::GetCurrentProcess(), (UINT)-1);
  }
  lpParentCallDataEntry = (CHookEngCallDataEntry*)(lpCallDataEntry->GetPrevEntry());
  /*
  while (1)
  {
    lpCallDataEntry = (CHookEngCallDataEntry*)(lpTlsData->sHookCallData.cInUseStack.GetTail());
    if (lpCallDataEntry == NULL)
      break;
#if defined _M_IX86
    if (lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Esp <= sAsmRegisters.sInteger.Esp)
      break;
#elif defined _M_X64
    if (lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Rsp <= sAsmRegisters.sInteger.Rsp)
      break;
#endif
    //remove calldata from in-use list and move to the free list
    lpCallDataEntry->RemoveNode();
    lpCallDataEntry->cFuncParams.Release();
    lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
  }
  //get parent entries
  while (1)
  {
    lpParentCallDataEntry = (CHookEngCallDataEntry*)(lpCallDataEntry->GetPrevEntry());
    if (lpParentCallDataEntry == NULL)
      break;
#if defined _M_IX86
    if (lpParentCallDataEntry->sCallState.sAsmRegisters.sInteger.Esp ==
            lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Esp ||
        lpParentCallDataEntry->sCallState.sAsmRegisters.sInteger.Esp > sAsmRegisters.sInteger.Esp)
      break;
#elif defined _M_X64
    if (lpParentCallDataEntry->sCallState.sAsmRegisters.sInteger.Rsp ==
            lpCallDataEntry->sCallState.sAsmRegisters.sInteger.Rsp ||
        lpParentCallDataEntry->sCallState.sAsmRegisters.sInteger.Rsp > sAsmRegisters.sInteger.Rsp)
      break;
#endif
    //remove calldata from in-use list and move to the free list
    lpCallDataEntry->RemoveNode();
    lpCallDataEntry->cFuncParams.Release();
    lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
    lpCallDataEntry = lpParentCallDataEntry;
  }
  NKT_ASSERT(lpCallDataEntry != NULL);
  if (lpCallDataEntry == NULL)
  {
    ::MessageBoxW(NULL, L"Invalid internal state (1). Terminating application...", L"Deviare2", MB_OK|MB_ICONASTERISK);
    ::TerminateProcess(::GetCurrentProcess(), (UINT)-1);
  }
  NKT_ASSERT(lpCallDataEntry->lpHookEntry == lpHookEntry);
  */
  //restore original stack values after calling the original function. some api like
  //kernelbase.dll!CreateFileW overwrites the stack with internal data
  CNktDvFunctionParamsCache::PostCall_RestoreOriginalStackValues(lpCallDataEntry->cFuncParams,
                                                                 &(lpCallDataEntry->sCallState.sPreCallAsmRegisters));
  //get function timing (2c)
  nTimeDiffMs = CHookEngCallDataEntry::CTiming::TimeDiff(cFunctionTimingTemp[0].nTimeMs,
                                                         lpCallDataEntry->cFunctionTiming[1].nTimeMs);
  nTimeDiffMs -= lpCallDataEntry->nChildOverHeadMs;
  if (nTimeDiffMs < 0.0000001)
    nTimeDiffMs = 0.0;
  //init call info data
  nRetAddr = lpCallDataEntry->nReturnAddr;
  lpCallDataEntry->sCallState.dwSkipCall = 0;
  lpCallDataEntry->sCallState.dwOsLastError = dwOsLastError;
  nktMemCopy(&(lpCallDataEntry->sCallState.sAsmRegisters), &sAsmRegisters, sizeof(sAsmRegisters));
  //call the callback
  bPostCallCalled = FALSE;
  hRes = S_OK; //assume all is ok if not doing a postcall
  if ((lpHookEntry->nFlags & HOOKENG_FLAG_CallPostCall) != 0 &&
      ((lpHookEntry->nFlags & HOOKENG_FLAG_DontCallOnLdrLock) == 0 ||
       nktDvNtIsLoaderLoaderLockHeld(FALSE) == FALSE))
  {
    sCallInfo.dwHookId = lpHookEntry->dwId;
    sCallInfo.nPhase = CNktDvHookEngine::phPostCall;
    sCallInfo.nCookie = lpCallDataEntry->nCookie;
    sCallInfo.lpDbFunc = lpHookEntry->lpDbFunc;
    sCallInfo.bAsyncCallbacks = ((lpHookEntry->nFlags & HOOKENG_FLAG_AsyncCallbacks) != 0) ? TRUE : FALSE;
    sCallInfo.dwThreadId = ::GetCurrentThreadId();
    sCallInfo.nCurrentTimeMs = lpCallDataEntry->nCurrentTimeMs;
    sCallInfo.nElapsedTimeMs = nTimeDiffMs;
    sCallInfo.nChildsElapsedTimeMs = lpCallDataEntry->nChildsElapsedTimeMs;
    sCallInfo.lpCallState = &(lpCallDataEntry->sCallState);
    sCallInfo.nKernelTimeMs = lpTlsData->sThreadTimes.nKernelTimeMs;
    sCallInfo.nUserTimeMs = lpTlsData->sThreadTimes.nUserTimeMs;
    sCallInfo.nCpuClockCycles = lpTlsData->sThreadTimes.nCpuClockCycles;
    sCallInfo.dwChainDepth = lpCallDataEntry->dwChainDepth;
    //but first call the custom handlers if they were provided
    if ((lpHookEntry->nFlags & HOOKENG_FLAG_DontCallCustomHandlersOnLdrLock) == 0 ||
        nktDvNtIsLoaderLoaderLockHeld(FALSE) == FALSE)
    {
      hRes = lpHookEntry->ProcessCustomHandler(&cCustomParams, &sCallInfo, lpCallDataEntry->cFuncParams->cParameters,
                                               lpCallDataEntry->cFuncParams->cResult,
                                               lpCallDataEntry->aIntercallCustomData, lpCallback);
    }
    //call the callback
    if (hRes == S_FALSE)
    {
      //a custom handler inform us not to send the message to spymgr
      hRes = S_OK;
    }
    else if (SUCCEEDED(hRes))
    {
      hRes = lpCallback->HEC_OnHookCalled(sCallInfo, lpCallDataEntry->aIntercallCustomData,
                                          lpCallDataEntry->cFuncParams, cCustomParams);
      bPostCallCalled = TRUE;
    }
  }
  //if an error occurs
  if (FAILED(hRes))
    lpCallback->HEC_OnError(hRes);
  //get function timing (2d)
  cFunctionTimingTemp[1].Initialize(liTimerFreq);
  if (lpParentCallDataEntry != NULL)
  {
    lpParentCallDataEntry->nChildsElapsedTimeMs += nTimeDiffMs;
    //----
    nTimeDiffMs = CHookEngCallDataEntry::CTiming::TimeDiff(cFunctionTimingTemp[1].nTimeMs,
                                                           cFunctionTimingTemp[0].nTimeMs);
    lpParentCallDataEntry->nChildOverHeadMs += nTimeDiffMs;
    nTimeDiffMs = CHookEngCallDataEntry::CTiming::TimeDiff(lpCallDataEntry->cFunctionTiming[1].nTimeMs,
                                                           lpCallDataEntry->cFunctionTiming[0].nTimeMs);
    lpParentCallDataEntry->nChildOverHeadMs += nTimeDiffMs;
  }
  //set thread last times
  lpTlsData->ThreadTimesSetLast(cFunctionTimingTemp[1].nKernelMs, cFunctionTimingTemp[1].nUserMs,
                                cFunctionTimingTemp[1].nCpuClockCycles);
  //remove calldata from in-use list and move to the free list
  lpCallDataEntry->RemoveNode();
  lpCallDataEntry->cFuncParams.Release();
  lpTlsData->sHookCallData.cFreeList.PushTail(lpCallDataEntry);
  if (bPostCallCalled != FALSE)
  {
    //update registers
    SaveRegisters(&(sCallInfo.lpCallState->sAsmRegisters), nStackPointer);
    dwOsLastError = sCallInfo.lpCallState->dwOsLastError;
  }
  //return the original return address
  NKT_DEBUGPRINTLNA(Nektra::dlHookEnginePostCall, ("%lu) HookEngine[PostCall-Exit]: Entry=%IXh, "
                    "RetAddr=%IXh", ::GetTickCount(), lpHookEntry, nRetAddr));
  return nRetAddr;
}

LPBYTE CNktDvHookEngine::AllocInjectedCode(__in LPVOID lpRefAddr)
{
  LPBYTE lpPtr, lpInjCodeAddr;
  SIZE_T i, k, nCount;
  INJCODE_BLOCK sNewBlock;
#if defined _M_X64
  //calculate min/max address
  ULONGLONG nMin, nMax;
  MEMORY_BASIC_INFORMATION sMbi;
#endif //_M_X64

  //we are inside CNktDvHookEngine's lock so this call is safe
  if (nInjectedCodeMaxSize == 0)
  {
#if defined _M_IX86
    lpInjCodeAddr = (LPBYTE)NktDvSuperHook_x86;
#elif defined _M_X64
    lpInjCodeAddr = (LPBYTE)NktDvSuperHook_x64;
#endif
    lpInjCodeAddr = HookEng_SkipJumpInstructions(lpInjCodeAddr);
    for (lpPtr=lpInjCodeAddr; ; lpPtr++)
    {
#if defined _M_IX86
      if (HookEng_ReadUnalignedSizeT(lpPtr) == 0xFFDDFFFF)
        break;
#elif defined _M_X64
      if (HookEng_ReadUnalignedSizeT(lpPtr) == 0xFFDDFFDDFFDDFFFF)
        break;
#endif
    }
    k = (SIZE_T)(lpPtr - lpInjCodeAddr);
    k = (k+31) & (~31);
    k += 2*sizeof(SIZE_T); //max stub size
    NKT_ASSERT(k < 65536);
    //http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
    k--;
    k |= k >> 1;
    k |= k >> 2;
    k |= k >> 4;
    k |= k >> 8;
    k |= k >> 16;
    nInjectedCodeMaxSize = k+1;
    if (nInjectedCodeMaxSize < sizeof(LPBYTE))
      nInjectedCodeMaxSize = sizeof(LPBYTE); //minimum is sizeof(LPBYTE) for free-list pointers
    NKT_ASSERT(nInjectedCodeMaxSize <= 65536);
  }
  //find a previously allocated block
#if defined _M_X64
  //calculate min/max address
  nMin = nMax = ((ULONGLONG)(SIZE_T)lpRefAddr) & (~65535ui64);
  if (nMin > 0x40000000ui64)
    nMin -= 0x40000000ui64;
  else
    nMin = 0ui64;
  if (nMax < 0xFFFFFFFFFFFFFFFFui64-0x40000000ui64)
    nMax += 0x40000000ui64;
  else
    nMax = 0xFFFFFFFFFFFFFFFFui64;
#endif //_M_X64
  nCount = cInjectedCodeBlocks.GetCount();
  for (i=0; i<nCount; i++)
  {
    if (cInjectedCodeBlocks[i].lpFirstFreeItem != NULL)
    {
#if defined _M_X64
      if ((ULONGLONG)(SIZE_T)(cInjectedCodeBlocks[i].lpBaseAddress) >= nMin &&
          (ULONGLONG)(SIZE_T)(cInjectedCodeBlocks[i].lpBaseAddress) < nMax)
      {
#endif //_M_X64
        break;
#if defined _M_X64
      }
#endif //_M_X64
    }
  }
  if (i >= nCount)
  {
    //allocate a new block if no free slot is found
    sNewBlock.nFreeItemsCount = 65536/nInjectedCodeMaxSize;
#if defined _M_IX86
    sNewBlock.lpBaseAddress = (LPBYTE)::VirtualAlloc(NULL, 65536, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#elif defined _M_X64
    sNewBlock.lpBaseAddress = NULL;
    while (nMin < nMax)
    {
      memset(&sMbi, 0, sizeof(sMbi));
      ::VirtualQuery((LPCVOID)nMin, &sMbi, sizeof(sMbi));
      if (sMbi.State == MEM_FREE)
      {
        sNewBlock.lpBaseAddress = (LPBYTE)::VirtualAlloc((LPVOID)nMin, 65536, MEM_RESERVE|MEM_COMMIT,
                                                         PAGE_EXECUTE_READWRITE);
        if (sNewBlock.lpBaseAddress != NULL)
          break;
      }
      nMin += 65536;
    }
#endif
    if (sNewBlock.lpBaseAddress == NULL)
      return NULL;
    //initialize free list
    lpPtr = sNewBlock.lpFirstFreeItem = sNewBlock.lpBaseAddress;
    for (i=0; i<sNewBlock.nFreeItemsCount-1; i++)
    {
      *((LPBYTE*)lpPtr) = lpPtr+nInjectedCodeMaxSize;
      lpPtr += nInjectedCodeMaxSize;
    }
    *((LPBYTE*)lpPtr) = NULL;
    //add to list
    if (cInjectedCodeBlocks.AddElement(&sNewBlock) == FALSE)
    {
      ::VirtualFree(sNewBlock.lpBaseAddress, 0, MEM_FREE);
      return NULL;
    }
    i = cInjectedCodeBlocks.GetCount()-1;
  }
  //now get a free subblock
  NKT_ASSERT(cInjectedCodeBlocks[i].nFreeItemsCount > 0);
  lpInjCodeAddr = cInjectedCodeBlocks[i].lpFirstFreeItem;
  //set new list header
  cInjectedCodeBlocks[i].lpFirstFreeItem = *((LPBYTE*)lpInjCodeAddr);
  (cInjectedCodeBlocks[i].nFreeItemsCount)--;
#ifdef _DEBUG
  if (cInjectedCodeBlocks[i].lpFirstFreeItem == NULL)
  {
    NKT_ASSERT(cInjectedCodeBlocks[i].nFreeItemsCount == 0);
  }
  else
  {
    NKT_ASSERT(cInjectedCodeBlocks[i].nFreeItemsCount > 0);
  }
#endif //_DEBUG
  //done
  return lpInjCodeAddr;
}

VOID CNktDvHookEngine::FreeInjectedCode(__in CHookEntry *lpHookEntry)
{
  //we are inside CNktDvHookEngine's lock so this call is safe
  SIZE_T i, k, nCount;

  if (lpHookEntry->lpInjectedCodeAddr != NULL)
  {
    nCount = cInjectedCodeBlocks.GetCount();
    for (i=0; i<nCount; i++)
    {
      if ((SIZE_T)(lpHookEntry->lpInjectedCodeAddr) >= (SIZE_T)(cInjectedCodeBlocks[i].lpBaseAddress))
      {
        k = (SIZE_T)(lpHookEntry->lpInjectedCodeAddr) - (SIZE_T)(cInjectedCodeBlocks[i].lpBaseAddress);
        if (k < 65536)
        {
          //got the block
          NKT_ASSERT((k % nInjectedCodeMaxSize) == 0);
          //set new free list header
          *((LPBYTE*)(lpHookEntry->lpInjectedCodeAddr)) = cInjectedCodeBlocks[i].lpFirstFreeItem;
          cInjectedCodeBlocks[i].lpFirstFreeItem = lpHookEntry->lpInjectedCodeAddr;
          //really free memory if empty
          if ((++(cInjectedCodeBlocks[i].nFreeItemsCount)) >= 65536/nInjectedCodeMaxSize)
          {
            ::VirtualFree(cInjectedCodeBlocks[i].lpBaseAddress, 0, MEM_FREE);
            cInjectedCodeBlocks.RemoveElementAt(i);
          }
          break;
        }
      }
    }
  }
  return;
}

//-----------------------------------------------------------
//-----------------------------------------------------------

static SIZE_T __stdcall PreCallCommon(__inout CNktDvHookEngine *lpEnginePtr, __inout LPVOID lpHookEntry,
                                      __in SIZE_T nStackPointer)
{
  SIZE_T nRes;
  DWORD dwOsErr;

  dwOsErr = ::GetLastError();
  nRes = 0; //ignore the call
  {
    CNktDvHookEngineAutoTlsLock cAutoTls;
    HRESULT hRes;

    hRes = cAutoTls.GetError();
    if (SUCCEEDED(hRes))
    {
      if (cAutoTls.IsAlreadyLocked() == FALSE)
        nRes = lpEnginePtr->PreCall(lpHookEntry, cAutoTls.GetData(), nStackPointer, dwOsErr);
    }
    else
    {
      //if cancelled => thread is exiting so assume that it is not an error
      //else inform error
      if (hRes != NKT_DVERR_Cancelled)
        lpEnginePtr->PreCall(NULL, NULL, 0, dwOsErr);
    }
  }
  ::SetLastError(dwOsErr);
  return nRes;
}

static SIZE_T __stdcall PostCallCommon(__inout CNktDvHookEngine *lpEnginePtr, __inout LPVOID lpHookEntry,
                                       __in SIZE_T nStackPointer)
{
  SIZE_T nRes;
  DWORD dwOsErr;

  dwOsErr = ::GetLastError();
  {
    CNktDvHookEngineAutoTlsLock cAutoTls;

    nRes = lpEnginePtr->PostCall(lpHookEntry, cAutoTls.GetData(), nStackPointer, dwOsErr);
  }
  ::SetLastError(dwOsErr);
  return nRes;
}

#if defined _M_IX86
static __inline VOID LoadRegisters(__out LPNKT_DV_ASMREGISTERS32 lpRegisters, __out PSIZE_T lpnReturnAddr,
                                   __in SIZE_T nStackPointer, __inout CHookEntry *lpHookEntry, __in BOOL bPreCall)
{
  DWORD dwTemp;

  lpRegisters->sInteger.Eip = (DWORD)(lpHookEntry->lpOrigProc);
  //lpRegisters->sInteger.Eip = (DWORD)(lpHookEntry->lpInjectedCodeAddr);
  dwTemp = *((DWORD*)(nStackPointer+0x00));
  lpRegisters->sInternalUseOnly.dwAligment = dwTemp & 0x7FFFFFFFUL;
  lpRegisters->sInternalUseOnly.Original_St0 =
    lpRegisters->sFloating.St0 = *((double*)(nStackPointer+0x04));
  nStackPointer += lpRegisters->sInternalUseOnly.dwAligment;
  lpRegisters->sInteger.Esp = nStackPointer+0x28;
  if (bPreCall == FALSE)
  {
    //preserve stack size for postcall plus some extra
    if (lpHookEntry->nStackReturnSize == NKT_SIZE_T_MAX)
      lpRegisters->sInteger.Esp += CALC_STACK_PRESERVE(DUMMY_CALC_STACK_PRESERVE_SIZE);
    else
      lpRegisters->sInteger.Esp += CALC_STACK_PRESERVE(lpHookEntry->nStackReturnSize);
    //add to the Esp register the size of the dummy value that will be modified with the real return value
    lpRegisters->sInteger.Esp += sizeof(DWORD);
  }
  lpRegisters->sInteger.Eax = *((DWORD*)(nStackPointer+0x24));
  lpRegisters->sInteger.Ebx = *((DWORD*)(nStackPointer+0x20));
  lpRegisters->sInteger.Ecx = *((DWORD*)(nStackPointer+0x1C));
  lpRegisters->sInteger.Edx = *((DWORD*)(nStackPointer+0x18));
  lpRegisters->sInteger.Ebp = *((DWORD*)(nStackPointer+0x14));
  lpRegisters->sInteger.Esi = *((DWORD*)(nStackPointer+0x10));
  lpRegisters->sInteger.Edi = *((DWORD*)(nStackPointer+0x0C));
  if (lpnReturnAddr != NULL)
    *lpnReturnAddr = *((PSIZE_T)(lpRegisters->sInteger.Esp));
  return;
}

static __inline VOID SaveRegisters(__in LPNKT_DV_ASMREGISTERS32 lpRegisters, __in SIZE_T nStackPointer)
{
  if (lpRegisters->sInternalUseOnly.Original_St0 != lpRegisters->sFloating.St0)
  {
    *((double*)(nStackPointer+0x04)) = lpRegisters->sFloating.St0;
    *((DWORD*)(nStackPointer+0x00)) |= 0x80000000; //set st0 in-use flag
  }
  nStackPointer += lpRegisters->sInternalUseOnly.dwAligment;
  *((DWORD*)(nStackPointer+0x24)) = lpRegisters->sInteger.Eax;
  *((DWORD*)(nStackPointer+0x20)) = lpRegisters->sInteger.Ebx;
  *((DWORD*)(nStackPointer+0x1C)) = lpRegisters->sInteger.Ecx;
  *((DWORD*)(nStackPointer+0x18)) = lpRegisters->sInteger.Edx;
  *((DWORD*)(nStackPointer+0x14)) = lpRegisters->sInteger.Ebp;
  *((DWORD*)(nStackPointer+0x10)) = lpRegisters->sInteger.Esi;
  *((DWORD*)(nStackPointer+0x0C)) = lpRegisters->sInteger.Edi;
  return;
}

#elif defined _M_X64

static __inline VOID LoadRegisters(__out LPNKT_DV_ASMREGISTERS64 lpRegisters, __out PSIZE_T lpnReturnAddr,
                                   __in SIZE_T nStackPointer, __inout CHookEntry *lpHookEntry, __in BOOL bPreCall)
{
  lpRegisters->sInteger.Rip = (DWORD64)(lpHookEntry->lpOrigProc);
  //lpRegisters->sInteger.Rip = (DWORD64)(lpHookEntry->lpInjectedCodeAddr);
  lpRegisters->sInteger.Rax = *((DWORD64*)(nStackPointer+0x20));
  lpRegisters->sInteger.Rbx = *((DWORD64*)(nStackPointer+0x28));
  lpRegisters->sInteger.Rcx = *((DWORD64*)(nStackPointer+0x30));
  lpRegisters->sInteger.Rdx = *((DWORD64*)(nStackPointer+0x38));
  lpRegisters->sInteger.Rbp = *((DWORD64*)(nStackPointer+0x40));
  lpRegisters->sInteger.Rsp = nStackPointer+0x228;
  if (bPreCall == FALSE)
  {
    //add to the Rsp register the size of the dummy value that will be modified with the real return value
    lpRegisters->sInteger.Rsp += sizeof(DWORD64);
  }
  lpRegisters->sInteger.Rsi = *((DWORD64*)(nStackPointer+0x48));
  lpRegisters->sInteger.Rdi = *((DWORD64*)(nStackPointer+0x50));
  lpRegisters->sInteger.R8  = *((DWORD64*)(nStackPointer+0x58));
  lpRegisters->sInteger.R9  = *((DWORD64*)(nStackPointer+0x60));
  lpRegisters->sInteger.R10 = *((DWORD64*)(nStackPointer+0x68));
  lpRegisters->sInteger.R11 = *((DWORD64*)(nStackPointer+0x70));
  _mm_store_sd(&(lpRegisters->sFloating.Xmm0), *((__m128d*)(nStackPointer+0xA0)));
  _mm_store_sd(&(lpRegisters->sFloating.Xmm1), *((__m128d*)(nStackPointer+0xB0)));
  _mm_store_sd(&(lpRegisters->sFloating.Xmm2), *((__m128d*)(nStackPointer+0xC0)));
  _mm_store_sd(&(lpRegisters->sFloating.Xmm3), *((__m128d*)(nStackPointer+0xD0)));
  lpRegisters->sInternalUseOnly.Original_Xmm0 = lpRegisters->sFloating.Xmm0;
  lpRegisters->sInternalUseOnly.Original_Xmm1 = lpRegisters->sFloating.Xmm1;
  lpRegisters->sInternalUseOnly.Original_Xmm2 = lpRegisters->sFloating.Xmm2;
  lpRegisters->sInternalUseOnly.Original_Xmm3 = lpRegisters->sFloating.Xmm3;
  if (lpnReturnAddr != NULL)
    *lpnReturnAddr = *((PSIZE_T)(lpRegisters->sInteger.Rsp));
  return;
}

static __inline VOID SaveRegisters(__in LPNKT_DV_ASMREGISTERS64 lpRegisters, __in SIZE_T nStackPointer)
{
  __declspec(align(16)) __m128d nTemp128;

  *((DWORD64*)(nStackPointer+0x20)) = lpRegisters->sInteger.Rax;
  *((DWORD64*)(nStackPointer+0x28)) = lpRegisters->sInteger.Rbx;
  *((DWORD64*)(nStackPointer+0x30)) = lpRegisters->sInteger.Rcx;
  *((DWORD64*)(nStackPointer+0x38)) = lpRegisters->sInteger.Rdx;
  *((DWORD64*)(nStackPointer+0x40)) = lpRegisters->sInteger.Rbp;
  *((DWORD64*)(nStackPointer+0x48)) = lpRegisters->sInteger.Rsi;
  *((DWORD64*)(nStackPointer+0x50)) = lpRegisters->sInteger.Rdi;
  *((DWORD64*)(nStackPointer+0x58)) = lpRegisters->sInteger.R8;
  *((DWORD64*)(nStackPointer+0x60)) = lpRegisters->sInteger.R9;
  *((DWORD64*)(nStackPointer+0x68)) = lpRegisters->sInteger.R10;
  *((DWORD64*)(nStackPointer+0x70)) = lpRegisters->sInteger.R11;
  if (lpRegisters->sInternalUseOnly.Original_Xmm0 != lpRegisters->sFloating.Xmm0)
  {
    nTemp128 = _mm_set_sd(lpRegisters->sFloating.Xmm0);
    nktMemCopy((__m128d*)(nStackPointer+0xA0), &nTemp128, sizeof(nTemp128));
  }
  if (lpRegisters->sInternalUseOnly.Original_Xmm1 != lpRegisters->sFloating.Xmm1)
  {
    nTemp128 = _mm_set_sd(lpRegisters->sFloating.Xmm1);
    nktMemCopy((__m128d*)(nStackPointer+0xB0), &nTemp128, sizeof(nTemp128));
  }
  if (lpRegisters->sInternalUseOnly.Original_Xmm2 != lpRegisters->sFloating.Xmm2)
  {
    nTemp128 = _mm_set_sd(lpRegisters->sFloating.Xmm2);
    nktMemCopy((__m128d*)(nStackPointer+0xC0), &nTemp128, sizeof(nTemp128));
  }
  if (lpRegisters->sInternalUseOnly.Original_Xmm3 != lpRegisters->sFloating.Xmm3)
  {
    nTemp128 = _mm_set_sd(lpRegisters->sFloating.Xmm3);
    nktMemCopy((__m128d*)(nStackPointer+0xD0), &nTemp128, sizeof(nTemp128));
  }
  return;
}

#endif

static BOOL SecureMemCopy(__in LPVOID lpDest, __in LPVOID lpSrc, __in SIZE_T nCount)
{
  BOOL b = FALSE;

  __try
  {
    nktMemCopy(lpDest, lpSrc, nCount);
    b = TRUE;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    b = FALSE;
  }
  return b;
}

static BOOL SecureMemIsDiff(__in const void *lpBuf1, __in const void *lpBuf2, __in SIZE_T nCount)
{
  BOOL b = FALSE;

  __try
  {
    b = (nktMemCompare(lpBuf1, lpBuf2, nCount) != 0) ? TRUE : FALSE;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    b = FALSE;
  }
  return b;
}

static BOOL SecureCheckStub(__in CHookEntry *lpHookEntry)
{
  BOOL b = FALSE;
  SIZE_T k;

  __try
  {
    b = (nktMemCompare(lpHookEntry->lpHookedAddr, lpHookEntry->aModifiedStub, 8) == 0) ? TRUE : FALSE;
    if (b != FALSE)
    {
      //check double hook
      for (k=0; k<8; k++)
      {
        if (lpHookEntry->lpInjectedCodeAddr[k] != 0x90)
        {
          b = FALSE;
          break;
        }
      }
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    b = FALSE;
  }
  return b;
}
