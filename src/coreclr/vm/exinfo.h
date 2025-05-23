// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//

//


#ifndef __ExInfo_h__
#define __ExInfo_h__
#if !defined(FEATURE_EH_FUNCLETS)

#include "exstatecommon.h"

typedef DPTR(class ExInfo) PTR_ExInfo;
class ExInfo
{
    friend class ThreadExceptionState;
    friend class ClrDataExceptionState;

public:

    BOOL    IsHeapAllocated()
    {
        LIMITED_METHOD_CONTRACT;
        return m_StackAddress != (void *) this;
    }

    void CopyAndClearSource(ExInfo *from);

    void UnwindExInfo(VOID* limit);

    // Q: Why does this thing take an EXCEPTION_RECORD rather than an ExceptionCode?
    // A: Because m_ExceptionCode and Ex_WasThrownByUs have to be kept
    //    in sync and this function needs the exception parms inside the record to figure
    //    out the "IsTagged" part.
    void SetExceptionCode(const EXCEPTION_RECORD *pCER);

    DWORD GetExceptionCode()
    {
        LIMITED_METHOD_CONTRACT;
        return m_ExceptionCode;
    }

public:  // @TODO: make more of these private!
    // Note: the debugger assumes that m_pThrowable is a strong
    // reference so it can check it for NULL with preemptive GC
    // enabled.
    OBJECTHANDLE    m_hThrowable;       // thrown exception
    PTR_Frame       m_pSearchBoundary;  // topmost frame for current managed frame group
private:
    DWORD           m_ExceptionCode;    // After a catch of a CLR exception, pointers/context are trashed.
public:
    PTR_EXCEPTION_REGISTRATION_RECORD m_pBottomMostHandler; // most recent EH record registered

    // Reference to the topmost handler we saw during an SO that goes past us
    PTR_EXCEPTION_REGISTRATION_RECORD m_pTopMostHandlerDuringSO;

    LPVOID              m_dEsp;             // Esp when  fault occurred, OR esp to restore on endcatch

    PTR_ExInfo          m_pPrevNestedInfo;  // pointer to nested info if are handling nested exception

    size_t*             m_pShadowSP;        // Zero this after endcatch

    PTR_EXCEPTION_RECORD    m_pExceptionRecord;
    PTR_EXCEPTION_POINTERS  m_pExceptionPointers;
    PTR_CONTEXT             m_pContext;

    // We have a rare case where (re-entry to the EE from an unmanaged filter) where we
    // need to create a new ExInfo ... but don't have a nested handler for it.  The handlers
    // use stack addresses to figure out their correct lifetimes.  This stack location is
    // used for that.  For most records, it will be the stack address of the ExInfo ... but
    // for some records, it will be a pseudo stack location -- the place where we think
    // the record should have been (except for the re-entry case).
    //
    //
    //
    void* m_StackAddress; // A pseudo or real stack location for this record.

#ifndef TARGET_UNIX
private:
    EHWatsonBucketTracker m_WatsonBucketTracker;
public:
    inline PTR_EHWatsonBucketTracker GetWatsonBucketTracker()
    {
        LIMITED_METHOD_CONTRACT;
        return PTR_EHWatsonBucketTracker(PTR_HOST_MEMBER_TADDR(ExInfo, this, m_WatsonBucketTracker));
    }
#endif

private:
    BOOL                    m_fDeliveredFirstChanceNotification;
public:
    inline BOOL DeliveredFirstChanceNotification()
    {
        LIMITED_METHOD_CONTRACT;

        return m_fDeliveredFirstChanceNotification;
    }

    inline void SetFirstChanceNotificationStatus(BOOL fDelivered)
    {
        LIMITED_METHOD_CONTRACT;

        m_fDeliveredFirstChanceNotification = fDelivered;
    }

    // Returns the exception tracker previous to the current
    inline PTR_ExInfo GetPreviousExceptionTracker()
    {
        LIMITED_METHOD_CONTRACT;

        return m_pPrevNestedInfo;
    }

    // Returns the throwable associated with the tracker
    inline OBJECTREF GetThrowable()
    {
        LIMITED_METHOD_CONTRACT;

        return (m_hThrowable != NULL)?ObjectFromHandle(m_hThrowable):NULL;
    }

    // Returns the throwble associated with the tracker as handle
    inline OBJECTHANDLE GetThrowableAsHandle()
    {
        LIMITED_METHOD_CONTRACT;

        return m_hThrowable;
    }

public:

    DebuggerExState     m_DebuggerExState;
    EHClauseInfo        m_EHClauseInfo;
    ExceptionFlags      m_ExceptionFlags;

#ifdef DEBUGGING_SUPPORTED
    EHContext           m_InterceptionContext;
    BOOL                m_ValidInterceptionContext;
#endif

#ifdef DACCESS_COMPILE
    void EnumMemoryRegions(CLRDataEnumMemoryFlags flags);
#endif

    void Init();
    ExInfo() DAC_EMPTY();

    void DestroyExceptionHandle();

private:
    // Don't allow this
    ExInfo& operator=(const ExInfo &from);
};

PTR_ExInfo GetEHTrackerForPreallocatedException(OBJECTREF oPreAllocThrowable, PTR_ExInfo pStartingEHTracker);

#else // !FEATURE_EH_FUNCLETS

#include "exceptionhandling.h"

enum RhEHClauseKind
{
    RH_EH_CLAUSE_TYPED = 0,
    RH_EH_CLAUSE_FAULT = 1,
    RH_EH_CLAUSE_FILTER = 2,
    RH_EH_CLAUSE_UNUSED = 3,
};

enum RhEHFrameType
{
    RH_EH_FIRST_FRAME = 1,
    RH_EH_FIRST_RETHROW_FRAME = 2,
};

struct RhEHClause
{
    RhEHClauseKind _clauseKind;
    unsigned _tryStartOffset;
    unsigned _tryEndOffset;
    BYTE *_filterAddress;
    BYTE *_handlerAddress;
    void *_pTargetType;
    BOOL _isSameTry;
};

enum class ExKind : uint8_t
{
    None = 0,
    Throw = 1,
    HardwareFault = 2,
    KindMask = 3,

    RethrowFlag = 4,

    SupersededFlag = 8,

    InstructionFaultFlag = 0x10
};

struct PAL_SEHException;

struct LastReportedFuncletInfo
{
    PCODE IP;
    TADDR FP;
    uint32_t Flags;
};

struct ExInfo
{
    struct DAC_EXCEPTION_POINTERS
    {
        PTR_EXCEPTION_RECORD    ExceptionRecord;
        PTR_CONTEXT             ContextRecord;
    };

    class StackRange
    {
    public:
        StackRange();
        void Reset();
        bool IsEmpty();
        bool IsSupersededBy(StackFrame sf);
        void CombineWith(StackFrame sfCurrent, StackRange* pPreviousRange);
        bool Contains(StackFrame sf);
        void ExtendUpperBound(StackFrame sf);
        void ExtendLowerBound(StackFrame sf);
        void TrimLowerBound(StackFrame sf);
        StackFrame GetLowerBound();
        StackFrame GetUpperBound();
        INDEBUG(bool IsDisjointWithAndLowerThan(StackRange* pOtherRange));
    private:
        INDEBUG(bool IsConsistent());

    private:
        // <TODO> can we use a smaller encoding? </TODO>
        StackFrame          m_sfLowBound;
        StackFrame          m_sfHighBound;
    };

    ExInfo(Thread *pThread, EXCEPTION_RECORD *pExceptionRecord, CONTEXT *pExceptionContext, ExKind exceptionKind);

    // Releases all the resources owned by the ExInfo
    void ReleaseResources();

    // Make debugger and profiler callbacks before and after an exception handler (catch, finally, filter) is called
    void MakeCallbacksRelatedToHandler(
        bool fBeforeCallingHandler,
        Thread*                pThread,
        MethodDesc*            pMD,
        EE_ILEXCEPTION_CLAUSE* pEHClause,
        DWORD_PTR              dwHandlerStartPC,
        StackFrame             sf);

    static void PopExInfos(Thread *pThread, void *targetSp);

    // Previous ExInfo in the chain of exceptions rethrown from their catch / finally handlers
    PTR_ExInfo     m_pPrevNestedInfo;
    // thrown exception object handle
    OBJECTHANDLE   m_hThrowable;
    // EXCEPTION_RECORD and CONTEXT_RECORD describing the exception and its location
    DAC_EXCEPTION_POINTERS m_ptrs;
    // Information for the funclet we are calling
    EHClauseInfo   m_EHClauseInfo;
    // Flags representing exception handling state (exception is rethrown, unwind has started, various debugger notifications sent etc)
    ExceptionFlags m_ExceptionFlags;
    // Set to TRUE when the first chance notification was delivered for the current exception
    BOOL           m_fDeliveredFirstChanceNotification;
    // Code of the current exception
    DWORD          m_ExceptionCode;
#ifdef DEBUGGING_SUPPORTED
    // Stores information necessary to intercept an exception
    DebuggerExState m_DebuggerExState;
#endif // DEBUGGING_SUPPORTED
    // Low and high bounds of the stack unwound by the exception.
    // In the new EH implementation, they are updated during 2nd pass only.
    StackRange      m_ScannedStackRange;

    // Padding to make the ExInfo offsets that the managed EH code needs to access
    // the same for debug / release and Unix / Windows.
#ifdef TARGET_UNIX
    // sizeof(EHWatsonBucketTracker)
    BYTE m_padding[2 * sizeof(void*) + sizeof(DWORD)];
#else // TARGET_UNIX
    EHWatsonBucketTracker m_WatsonBucketTracker;
#ifndef _DEBUG
    //  sizeof(EHWatsonBucketTracker::m_DebugFlags)
    BYTE m_padding[sizeof(DWORD)];
#endif // _DEBUG
#endif // TARGET_UNIX

    // Context used by the stack frame iterator
    CONTEXT* m_pExContext;
    // actual exception object reference
    OBJECTREF m_exception;
    // Kind of the exception (software, hardware, rethrown)
    ExKind m_kind;
    // Exception handling pass (1 or 2)
    uint8_t m_passNumber;
    // Index of the current exception handling clause
    uint32_t m_idxCurClause;
    // Stack frame iterator used to walk stack frames while handling the exception
    StackFrameIterator m_frameIter;
    volatile size_t m_notifyDebuggerSP;
    // Initial explicit frame
    Frame* m_pFrame;

    // Stack frame of the caller of the currently running exception handling clause (catch, finally, filter)
    CallerStackFrame    m_csfEHClause;
    // Stack frame of the caller of the code that encloses the currently running exception handling clause
    CallerStackFrame    m_csfEnclosingClause;
    // Stack frame of the caller of the catch handler
    StackFrame          m_sfCallerOfActualHandlerFrame;
    // The exception handling clause for the catch handler that was identified during pass 1
    EE_ILEXCEPTION_CLAUSE m_ClauseForCatch;

#ifdef TARGET_UNIX
    // Set to TRUE to take ownership of the EXCEPTION_RECORD and CONTEXT_RECORD in the m_ptrs. When set, the
    // memory of those records is freed using PAL_FreeExceptionRecords when the ExInfo is destroyed.
    BOOL m_fOwnsExceptionPointers;
    // Exception propagation callback and context for ObjectiveC exception propagation support
    void(*m_propagateExceptionCallback)(void* context);
    void *m_propagateExceptionContext;
#endif // TARGET_UNIX

    // The following fields are for profiler / debugger use only
    EE_ILEXCEPTION_CLAUSE m_CurrentClause;
    // Method to report to the debugger / profiler when stack frame iterator leaves a frame
    MethodDesc    *m_pMDToReportFunctionLeave;
    // CONTEXT and REGDISPLAY used by the StackFrameIterator for stack walking
    CONTEXT        m_exContext;
    REGDISPLAY     m_regDisplay;
    // Initial explicit frame for stack walking
    Frame         *m_pInitialFrame;
    // Info on the last reported funclet used to report references in the parent frame
    LastReportedFuncletInfo m_lastReportedFunclet;

#ifdef TARGET_WINDOWS
    // Longjmp buffer used to restart longjmp after a block of managed frames when
    // longjmp jumps over them. This is possible on Windows only due to the way the
    // longjmp is implemented.
    jmp_buf       *m_pLongJmpBuf;
    int            m_longJmpReturnValue;
#endif

#if defined(TARGET_UNIX)
    void TakeExceptionPointersOwnership(PAL_SEHException* ex);
#endif // TARGET_UNIX

public:
#ifndef TARGET_UNIX
    inline PTR_EHWatsonBucketTracker GetWatsonBucketTracker()
    {
        LIMITED_METHOD_CONTRACT;
        return PTR_EHWatsonBucketTracker(PTR_HOST_MEMBER_TADDR(ExInfo, this, m_WatsonBucketTracker));
    }
#endif // !TARGET_UNIX

    // Returns the exception tracker previous to the current
    inline PTR_ExInfo GetPreviousExceptionTracker()
    {
        LIMITED_METHOD_CONTRACT;

        return m_pPrevNestedInfo;
    }

    inline OBJECTREF GetThrowable()
    {
        CONTRACTL
        {
            MODE_COOPERATIVE;
            NOTHROW;
            GC_NOTRIGGER;
        }
        CONTRACTL_END;

        if (0 != m_hThrowable)
        {
            return ObjectFromHandle(m_hThrowable);
        }

        return NULL;
    }

   inline BOOL DeliveredFirstChanceNotification()
   {
       LIMITED_METHOD_CONTRACT;

       return m_fDeliveredFirstChanceNotification;
   }

   inline void SetFirstChanceNotificationStatus(BOOL fDelivered)
   {
       LIMITED_METHOD_CONTRACT;

       m_fDeliveredFirstChanceNotification = fDelivered;
   }

   DWORD GetExceptionCode()
   {
       return m_ExceptionCode;
   }

   StackRange GetScannedStackRange()
   {
       LIMITED_METHOD_CONTRACT;

       return m_ScannedStackRange;
   }

    bool IsInFirstPass()
    {
        return !m_ExceptionFlags.UnwindHasStarted();
    }

#ifndef DACCESS_COMPILE
    void DestroyExceptionHandle()
    {
        // Never, ever destroy a preallocated exception handle.
        if ((m_hThrowable != NULL) && !CLRException::IsPreallocatedExceptionHandle(m_hThrowable))
        {
            DestroyHandle(m_hThrowable);
        }

        m_hThrowable = NULL;
    }
#endif // !DACCESS_COMPILE

#ifdef DACCESS_COMPILE
    void EnumMemoryRegions(CLRDataEnumMemoryFlags flags);
#endif
 
public:
    static OBJECTREF CreateThrowable(
        PEXCEPTION_RECORD pExceptionRecord,
        BOOL bAsynchronousThreadStop
        );

    INDEBUG(static UINT_PTR DebugComputeNestingLevel());

    // Return a StackFrame of the current frame for parent frame checking purposes.
    // Don't use this StackFrame in any way except to pass it back to the ExceptionTracker
    // via IsUnwoundToTargetParentFrame().
    static StackFrame GetStackFrameForParentCheck(CrawlFrame * pCF);

    static bool IsInStackRegionUnwoundBySpecifiedException(CrawlFrame * pCF, PTR_ExInfo pExceptionTracker);
    static bool IsInStackRegionUnwoundByCurrentException(CrawlFrame * pCF);

    static bool HasFrameBeenUnwoundByAnyActiveException(CrawlFrame * pCF);

    // Determines if we have unwound to the specified parent method frame.
    // Currently this is only used for funclet skipping.
    static bool IsUnwoundToTargetParentFrame(CrawlFrame * pCF, StackFrame sfParent);
    static bool IsUnwoundToTargetParentFrame(StackFrame sfToCheck, StackFrame sfParent);

    // Given the CrawlFrame for a funclet frame, return the frame pointer of the enclosing funclet frame.
    // For filter funclet frames and a normal method frames, this function returns a NULL StackFrame.
    //
    // <WARNING>
    // It is not valid to call this function on an arbitrary funclet.  You have to be doing a full stackwalk from
    // the leaf frame and skipping method frames as indicated by the return value of this function.  This function
    // relies on the ExInfos, which are collapsed in the second pass when a nested exception escapes.
    // When this happens, we'll lose information on the funclet represented by the collapsed tracker.
    // </WARNING>
    //
    // Return Value:
    // StackFrame.IsNull()   - no skipping is necessary
    // StackFrame.IsMaxVal() - skip one frame and then ask again
    // Anything else         - skip to the method frame indicated by the return value and ask again
    static StackFrame FindParentStackFrameForStackWalk(CrawlFrame* pCF, bool fForGCReporting = false);

    // Given the CrawlFrame for a filter funclet frame, return the frame pointer of the parent method frame.
    // It also returns the relative offset and the caller SP of the parent method frame.
    //
    // <WARNING>
    // The same warning for FindParentStackFrameForStackWalk() also applies here.  Moreoever, although
    // this function seems to be more convenient, it may potentially trigger a full stackwalk!  Do not
    // call this unless you know absolutely what you are doing.  In most cases FindParentStackFrameForStackWalk()
    // is what you need.
    // </WARNING>
    //
    // Return Value:
    // StackFrame.IsNull()   - no skipping is necessary
    // Anything else         - the StackFrame of the parent method frame
    static StackFrame FindParentStackFrameEx(CrawlFrame* pCF,
                                             DWORD*      pParentOffset);

    static void
        PopTrackers(StackFrame sfResumeFrame,
                    bool fPopWhenEqual);

    static void
        PopTrackers(void* pvStackPointer);

    static void UpdateNonvolatileRegisters(T_CONTEXT* pContextRecord, REGDISPLAY *pRegDisplay, bool fAborting);

private:
    // private helpers
    static StackFrame GetCallerSPOfParentOfNonExceptionallyInvokedFunclet(CrawlFrame *pCF);

    static StackFrame FindParentStackFrameHelper(CrawlFrame* pCF,
                                                 bool*       pfRealParent,
                                                 DWORD*      pParentOffset,
                                                 bool        fForGCReporting = false);

    static StackFrame RareFindParentStackFrame(CrawlFrame* pCF,
                                               DWORD*      pParentOffset);

    static StackWalkAction RareFindParentStackFrameCallback(CrawlFrame* pCF, LPVOID pData);
};

#endif // !FEATURE_EH_FUNCLETS
#endif // __ExInfo_h__
