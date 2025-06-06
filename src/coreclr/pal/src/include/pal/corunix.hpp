// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*++



Module Name:

    corunix.hpp

Abstract:

    Internal interface and object definitions



--*/

#ifndef _CORUNIX_H
#define _CORUNIX_H

#include "palinternal.h"

namespace CorUnix
{
    typedef DWORD PAL_ERROR;

    //
    // Forward declarations for classes defined in other headers
    //

    class CPalThread;

    //
    // Forward declarations for items in this header
    //

    class CObjectType;
    class IPalObject;

    //
    // A simple counted string class. Using counted strings
    // allows for some optimizations when searching for a matching string.
    //

    class CPalString
    {
    protected:

        const WCHAR *m_pwsz;    // NULL terminated

        //
        // Length of string, not including terminating NULL
        //

        DWORD m_dwStringLength;

        //
        // Length of buffer backing string; must be at least 1+dwStringLength
        //

        DWORD m_dwMaxLength;

    public:

        CPalString()
            :
            m_pwsz(NULL),
            m_dwStringLength(0),
            m_dwMaxLength(0)
        {
        };

        CPalString(
            const WCHAR *pwsz
            )
        {
            SetString(pwsz);
        };

        void
        SetString(
            const WCHAR *pwsz
            )
        {
            SetStringWithLength(pwsz, PAL_wcslen(pwsz));
        };

        void
        SetStringWithLength(
            const WCHAR *pwsz,
            DWORD dwStringLength
            )
        {
            m_pwsz = pwsz;
            m_dwStringLength = dwStringLength;
            m_dwMaxLength = m_dwStringLength + 1;

        };

        PAL_ERROR
        CopyString(
            CPalString *psSource
            );

        void
        FreeBuffer();

        const WCHAR *
        GetString()
        {
            return m_pwsz;
        };

        DWORD
        GetStringLength()
        {
            return m_dwStringLength;
        };

        DWORD
        GetMaxLength()
        {
            return m_dwMaxLength;
        };

    };

    //
    // Signature of the cleanup routine that is to be called for an object
    // type when:
    // 1) The object's refcount drops to 0
    // 2) A process is shutting down
    //
    // When the third parameter (fShutdown) is TRUE the process is in
    // the act of exiting. The cleanup routine should not perform any
    // unnecessary cleanup operations (e.g., closing file descriptors,
    // since the OS will automatically close them when the process exits)
    // in this situation.
    //

    typedef void (*OBJECTCLEANUPROUTINE) (
        CPalThread *,   // pThread
        IPalObject *,   // pObjectToCleanup
        bool            // fShutdown
        );

    typedef void (*OBJECT_IMMUTABLE_DATA_COPY_ROUTINE) (
        void *,
        void *);
    typedef void (*OBJECT_IMMUTABLE_DATA_CLEANUP_ROUTINE) (
        void *);
    typedef void (*OBJECT_PROCESS_LOCAL_DATA_CLEANUP_ROUTINE) (
        CPalThread *,   // pThread
        IPalObject *);

    enum PalObjectTypeId
    {
        otiAutoResetEvent = 0,
        otiManualResetEvent,
        otiMutex,
        otiNamedMutex,
        otiSemaphore,
        otiFile,
        otiFileMapping,
        otiSocket,
        otiProcess,
        otiThread,
        otiIOCompletionPort,
        ObjectTypeIdCount    // This entry must come last in the enumeration
    };

    //
    // There should be one instance of CObjectType for each supported
    // type in a process; this allows for pointer equality tests
    // to be used (though in general it's probably better to use
    // checks based on the type ID). All members of this structure are
    // immutable.
    //
    // The data size members control how much space will be allocated for
    // instances of this object. Any or all of those members may be 0.
    //
    // dwSupportedAccessRights is the mask of valid access bits for this
    // object type. Supported generic rights should not be included in
    // this member.
    //
    // The generic access rights mapping (structure TBD) defines how the
    // supported generic access rights (e.g., GENERIC_READ) map to the
    // specific access rights for this object type.
    //
    // If the object may be waited on eSynchronizationSupport should be
    // WaitableObject. (Note that this implies that object type supports
    // the SYNCHRONIZE access right.)
    //
    // The remaining members describe the wait-object semantics for the
    // object type when eSynchronizationSupport is WaitableObject:
    //
    // * eSignalingSemantics: SingleTransitionObject for objects that, once
    //   they transition to the signaled state, can never transition back to
    //   the unsignaled state (e.g., processes and threads)
    //
    // * eThreadReleaseSemantics: if ThreadReleaseAltersSignalCount the object's
    //   signal count is decremented when a waiting thread is released; otherwise,
    //   the signal count is not modified (as is desired for a manual reset event).
    //   Must be ThreadReleaseHasNoSideEffects if eSignalingSemantics is
    //   SingleTransitionObject
    //
    // * eOwnershipSemantics: OwnershipTracked only for mutexes, for which the
    //   previous two items must also ObjectCanBeUnsignaled and
    //   ThreadReleaseAltersSignalCount.
    //

    class CObjectType
    {
    public:
        enum SynchronizationSupport
        {
            WaitableObject,
            UnwaitableObject
        };

        enum SignalingSemantics
        {
            ObjectCanBeUnsignaled,
            SingleTransitionObject,
            SignalingNotApplicable
        };

        enum ThreadReleaseSemantics
        {
            ThreadReleaseAltersSignalCount,
            ThreadReleaseHasNoSideEffects,
            ThreadReleaseNotApplicable
        };

        enum OwnershipSemantics
        {
            OwnershipTracked,
            NoOwner,
            OwnershipNotApplicable
        };

    private:

        //
        // Array that maps object type IDs to the corresponding
        // CObjectType instance
        //

        static CObjectType* s_rgotIdMapping[];

        PalObjectTypeId m_eTypeId;
        OBJECTCLEANUPROUTINE m_pCleanupRoutine;
        DWORD m_dwImmutableDataSize;
        OBJECT_IMMUTABLE_DATA_COPY_ROUTINE m_pImmutableDataCopyRoutine;
        OBJECT_IMMUTABLE_DATA_CLEANUP_ROUTINE m_pImmutableDataCleanupRoutine;
        DWORD m_dwProcessLocalDataSize;
        OBJECT_PROCESS_LOCAL_DATA_CLEANUP_ROUTINE m_pProcessLocalDataCleanupRoutine;
        // Generic access rights mapping
        SynchronizationSupport m_eSynchronizationSupport;
        SignalingSemantics m_eSignalingSemantics;
        ThreadReleaseSemantics m_eThreadReleaseSemantics;
        OwnershipSemantics m_eOwnershipSemantics;

    public:

        CObjectType(
            PalObjectTypeId eTypeId,
            OBJECTCLEANUPROUTINE pCleanupRoutine,
            DWORD dwImmutableDataSize,
            OBJECT_IMMUTABLE_DATA_COPY_ROUTINE pImmutableDataCopyRoutine,
            OBJECT_IMMUTABLE_DATA_CLEANUP_ROUTINE pImmutableDataCleanupRoutine,
            DWORD dwProcessLocalDataSize,
            OBJECT_PROCESS_LOCAL_DATA_CLEANUP_ROUTINE pProcessLocalDataCleanupRoutine,
            SynchronizationSupport eSynchronizationSupport,
            SignalingSemantics eSignalingSemantics,
            ThreadReleaseSemantics eThreadReleaseSemantics,
            OwnershipSemantics eOwnershipSemantics
            )
            :
            m_eTypeId(eTypeId),
            m_pCleanupRoutine(pCleanupRoutine),
            m_dwImmutableDataSize(dwImmutableDataSize),
            m_pImmutableDataCopyRoutine(pImmutableDataCopyRoutine),
            m_pImmutableDataCleanupRoutine(pImmutableDataCleanupRoutine),
            m_dwProcessLocalDataSize(dwProcessLocalDataSize),
            m_pProcessLocalDataCleanupRoutine(pProcessLocalDataCleanupRoutine),
            m_eSynchronizationSupport(eSynchronizationSupport),
            m_eSignalingSemantics(eSignalingSemantics),
            m_eThreadReleaseSemantics(eThreadReleaseSemantics),
            m_eOwnershipSemantics(eOwnershipSemantics)
        {
            s_rgotIdMapping[eTypeId] = this;
        };

        static
        CObjectType *
        GetObjectTypeById(
            PalObjectTypeId otid
            )
        {
            return s_rgotIdMapping[otid];
        };

        PalObjectTypeId
        GetId(
            void
            )
        {
            return m_eTypeId;
        };

        OBJECTCLEANUPROUTINE
        GetObjectCleanupRoutine(
            void
            )
        {
            return m_pCleanupRoutine;
        };

        DWORD
        GetImmutableDataSize(
            void
            )
        {
            return  m_dwImmutableDataSize;
        };

        void
        SetImmutableDataCopyRoutine(
            OBJECT_IMMUTABLE_DATA_COPY_ROUTINE ptr
            )
        {
            m_pImmutableDataCopyRoutine = ptr;
        };

        OBJECT_IMMUTABLE_DATA_COPY_ROUTINE
        GetImmutableDataCopyRoutine(
            void
            )
        {
            return m_pImmutableDataCopyRoutine;
        };

        void
        SetImmutableDataCleanupRoutine(
            OBJECT_IMMUTABLE_DATA_CLEANUP_ROUTINE ptr
            )
        {
            m_pImmutableDataCleanupRoutine = ptr;
        };

        OBJECT_IMMUTABLE_DATA_CLEANUP_ROUTINE
        GetImmutableDataCleanupRoutine(
            void
            )
        {
            return m_pImmutableDataCleanupRoutine;
        }

        DWORD
        GetProcessLocalDataSize(
            void
            )
        {
            return m_dwProcessLocalDataSize;
        };

        OBJECT_PROCESS_LOCAL_DATA_CLEANUP_ROUTINE
        GetProcessLocalDataCleanupRoutine(
            void
            )
        {
            return m_pProcessLocalDataCleanupRoutine;
        }

        // Generic access rights mapping

        SynchronizationSupport
        GetSynchronizationSupport(
            void
            )
        {
            return  m_eSynchronizationSupport;
        };

        SignalingSemantics
        GetSignalingSemantics(
            void
            )
        {
            return  m_eSignalingSemantics;
        };

        ThreadReleaseSemantics
        GetThreadReleaseSemantics(
            void
            )
        {
            return  m_eThreadReleaseSemantics;
        };

        OwnershipSemantics
        GetOwnershipSemantics(
            void
            )
        {
            return  m_eOwnershipSemantics;
        };
    };

    class CAllowedObjectTypes
    {
    private:

        bool m_rgfAllowedTypes[ObjectTypeIdCount];

    public:

        bool
        IsTypeAllowed(PalObjectTypeId eTypeId);

        //
        // Constructor for multiple allowed types
        //

        CAllowedObjectTypes(
            PalObjectTypeId rgAllowedTypes[],
            DWORD dwAllowedTypeCount
            );

        //
        // Single allowed type constructor
        //

        CAllowedObjectTypes(
            PalObjectTypeId eAllowedType
            );

        //
        // Allow all types or no types constructor
        //

        CAllowedObjectTypes(
            bool fAllowAllObjectTypes
            )
        {
            for (DWORD dw = 0; dw < ObjectTypeIdCount; dw += 1)
            {
                m_rgfAllowedTypes[dw] = fAllowAllObjectTypes;
            }
        };

        ~CAllowedObjectTypes()
        {
        };
    };

    //
    // Attributes for a given object instance. If the object does not have
    // a name the sObjectName member should be zero'd out. If the default
    // security attributes are desired then pSecurityAttributes should
    // be NULL.
    //

    class CObjectAttributes
    {
    public:

        CPalString sObjectName;
        LPSECURITY_ATTRIBUTES pSecurityAttributes;

        CObjectAttributes(
            const WCHAR *pwszObjectName,
            LPSECURITY_ATTRIBUTES pSecurityAttributes_
            )
            :
            pSecurityAttributes(pSecurityAttributes_)
        {
            if (NULL != pwszObjectName)
            {
                sObjectName.SetString(pwszObjectName);
            }
        };

        CObjectAttributes()
            :
            pSecurityAttributes(NULL)
        {
        };
    };

    //
    // ISynchStateController is used to modify any object's synchronization
    // state. It is intended to be used from within the APIs exposed for
    // various objects (e.g., SetEvent, ReleaseMutex, etc.).
    //
    // Each ISynchStateController instance implicitly holds what should be
    // viewed as the global dispatcher lock, and as such should be released
    // as quickly as possible. An ISynchStateController instance is bound to
    // the thread that requested it; it may not be passed to a different
    // thread.
    //

    class ISynchStateController
    {
    public:

        virtual
        PAL_ERROR
        GetSignalCount(
            LONG *plSignalCount
            ) = 0;

        virtual
        PAL_ERROR
        SetSignalCount(
            LONG lNewCount
            ) = 0;

        virtual
        PAL_ERROR
        IncrementSignalCount(
            LONG lAmountToIncrement
            ) = 0;

        virtual
        PAL_ERROR
        DecrementSignalCount(
            LONG lAmountToDecrement
            ) = 0;

        //
        // The following two routines may only be used for object types
        // where eOwnershipSemantics is OwnershipTracked (i.e., mutexes).
        //

        //
        // SetOwner is intended to be used in the implementation of
        // CreateMutex when bInitialOwner is TRUE. It must be called
        // before the new object instance is registered with the
        // handle manager. Any other call to this method is an error.
        //

        virtual
        PAL_ERROR
        SetOwner(
            CPalThread *pNewOwningThread
            ) = 0;

        //
        // DecrementOwnershipCount returns an error if the object
        // is unowned, or if the thread this controller is bound to
        // is not the owner of the object.
        //

        virtual
        PAL_ERROR
        DecrementOwnershipCount(
            void
            ) = 0;

        virtual
        void
        ReleaseController(
            void
            ) = 0;
    };

    //
    // ISynchWaitController is used to indicate a thread's desire to wait for
    // an object (which possibly includes detecting instances where the wait
    // can be satisfied without blocking). It is intended to be used by object
    // wait function (WaitForSingleObject, etc.).
    //
    // Each ISynchWaitController instance implicitly holds what should be
    // viewed as the global dispatcher lock, and as such should be released
    // as quickly as possible. An ISynchWaitController instance is bound to
    // the thread that requested it; it may not be passed to a different
    // thread.
    //
    // A thread may hold multiple ISynchWaitController instances
    // simultaneously.
    //

    enum WaitType
    {
        SingleObject,
        MultipleObjectsWaitOne,
        MultipleObjectsWaitAll
    };

    class ISynchWaitController
    {
    public:

        //
        // CanThreadWaitWithoutBlocking informs the caller if a wait
        // operation may succeed immediately, but does not actually
        // alter any object state. ReleaseWaitingThreadWithoutBlocking
        // alters the object state, and will return an error if it is
        // not possible for the wait to be immediately satisfied.
        //

        virtual
        PAL_ERROR
        CanThreadWaitWithoutBlocking(
            bool *pfCanWaitWithoutBlocking,     // OUT
            bool *pfAbandoned
            ) = 0;

        virtual
        PAL_ERROR
        ReleaseWaitingThreadWithoutBlocking(
            ) = 0;

        //
        // dwIndex is intended for MultipleObjectsWaitOne situations. The
        // index for the object that becomes signaled and satisfies the
        // wait will be returned in the call to BlockThread.
        //

        virtual
        PAL_ERROR
        RegisterWaitingThread(
            WaitType eWaitType,
            DWORD dwIndex,
            bool fAltertable,
            bool fPrioritize
            ) = 0;

        //
        // Why is there no unregister waiting thread routine? Unregistration
        // is the responsibility of the synchronization provider, not the
        // implementation of the wait object routines. (I can be convinced
        // that this isn't the best approach, though...)
        //

        virtual
        void
        ReleaseController(
            void
            ) = 0;
    };

    enum LockType
    {
        ReadLock,
        WriteLock
    };

    class IDataLock
    {
    public:

        //
        // If a thread obtains a write lock but does not actually
        // modify any data it should set fDataChanged to FALSE. If
        // a thread obtain a read lock and does actually modify any
        // data it should be taken out back and shot.
        //

        virtual
        void
        ReleaseLock(
            CPalThread *pThread,                // IN, OPTIONAL
            bool fDataChanged
            ) = 0;
    };

    class IPalObject
    {
    public:

        virtual
        CObjectType *
        GetObjectType(
            VOID
            ) = 0;

        virtual
        CObjectAttributes *
        GetObjectAttributes(
            VOID
            ) = 0;

        virtual
        PAL_ERROR
        GetImmutableData(
            void **ppvImmutableData             // OUT
            ) = 0;

        //
        // The following two routines obtain either a read or write
        // lock on the data in question. If a thread needs to examine
        // both process-local and shared data simultaneously it must obtain
        // the shared data first. A thread may not hold data locks
        // on two different objects at the same time.
        //

        virtual
        PAL_ERROR
        GetProcessLocalData(
            CPalThread *pThread,                // IN, OPTIONAL
            LockType eLockRequest,
            IDataLock **ppDataLock,             // OUT
            void **ppvProcessLocalData          // OUT
            ) = 0;

        //
        // The following two routines obtain the global dispatcher lock.
        // If a thread needs to make use of a synchronization interface
        // and examine object data it must obtain the synchronization
        // interface first. A thread is allowed to hold synchronization
        // interfaces for multiple objects at the same time if it obtains
        // all of the interfaces through a single call (see IPalSynchronizationManager
        // below).
        //
        // The single-call restriction allows the underlying implementation
        // to possibly segement the global dispatcher lock. If this restriction
        // were not in place (i.e., if a single thread were allowed to call
        // GetSynchXXXController for multiple objects) no such segmentation
        // would be possible as there would be no way know in what order a
        // thread would choose to obtain the controllers.
        //
        // Note: this design precludes simultaneous acquisition of both
        // the state and wait controller for an object but there are
        // currently no places where doing so would be necessary.
        //

        virtual
        PAL_ERROR
        GetSynchStateController(
            CPalThread *pThread,                // IN, OPTIONAL
            ISynchStateController **ppStateController   // OUT
            ) = 0;

        virtual
        PAL_ERROR
        GetSynchWaitController(
            CPalThread *pThread,                // IN, OPTIONAL
            ISynchWaitController **ppWaitController   // OUT
            ) = 0;

        virtual
        DWORD
        AddReference(
            void
            ) = 0;

        virtual
        DWORD
        ReleaseReference(
            CPalThread *pThread
            ) = 0;

        //
        // This routine is only for use by the synchronization manager
        // (specifically, for GetSynch*ControllersForObjects). The
        // caller must have acquired the appropriate lock before
        // (whatever exactly that must be) before calling this routine.
        //

        virtual
        PAL_ERROR
        GetObjectSynchData(
            VOID **ppvSynchData             // OUT
            ) = 0;

    };

    class IPalProcess
    {
    public:
        virtual
        DWORD
        GetProcessID(
            void
            ) = 0;
    };

    class IPalObjectManager
    {
    public:

        //
        // Object creation (e.g., what is done by CreateEvent) is a two step
        // process. First, the new object is allocated and the initial
        // properties set (e.g., initially signaled). Next, the object is
        // registered, yielding a handle. If an object of the same name
        // and appropriate type already existed the returned handle will refer
        // to the previously existing object, and the newly allocated object
        // will have been thrown away.
        //
        // (The two phase process minimizes the amount of time that any
        // namespace locks need to be held. While some wasted work may be
        // done in the existing object case that work only impacts the calling
        // thread. Checking first for existence and then allocating and
        // initializing on failure requires any namespace lock to be held for
        // a much longer period of time, impacting the entire system.)
        //

        virtual
        PAL_ERROR
        AllocateObject(
            CPalThread *pThread,                // IN, OPTIONAL
            CObjectType *pType,
            CObjectAttributes *pAttributes,
            IPalObject **ppNewObject            // OUT
            ) = 0;

        //
        // After calling RegisterObject pObjectToRegister is no
        // longer valid. If successful there are two references
        // on the returned object -- one for the handle, and one
        // for the instance returned in ppRegisteredObject. The
        // caller, therefore, is responsible for releasing the
        // latter.
        //
        // For named object pAllowedTypes specifies what type of
        // existing objects can be returned in ppRegisteredObjects.
        // This is primarily intended for CreateEvent, so that
        // a ManualResetEvent can be returned when attempting to
        // register an AutoResetEvent (and vice-versa). pAllowedTypes
        // must include the type of pObjectToRegister.
        //

        virtual
        PAL_ERROR
        RegisterObject(
            CPalThread *pThread,                // IN, OPTIONAL
            IPalObject *pObjectToRegister,
            CAllowedObjectTypes *pAllowedTypes,
            HANDLE *pHandle,                    // OUT
            IPalObject **ppRegisteredObject     // OUT
            ) = 0;

        //
        // LocateObject is used for OpenXXX routines. ObtainHandleForObject
        // is needed for the OpenXXX routines and DuplicateHandle.
        //

        virtual
        PAL_ERROR
        LocateObject(
            CPalThread *pThread,                // IN, OPTIONAL
            CPalString *psObjectToLocate,
            CAllowedObjectTypes *pAllowedTypes,
            IPalObject **ppObject               // OUT
            ) = 0;

        //
        // pProcessForHandle is to support cross-process handle
        // duplication. It only needs to be specified when acquiring
        // a handle meant for use in a different process; it should
        // be left NULL when acquiring a handle for the current
        // process.
        //

        virtual
        PAL_ERROR
        ObtainHandleForObject(
            CPalThread *pThread,                // IN, OPTIONAL
            IPalObject *pObject,
            HANDLE *pNewHandle                  // OUT
            ) = 0;

        virtual
        PAL_ERROR
        RevokeHandle(
            CPalThread *pThread,                // IN, OPTIONAL
            HANDLE hHandleToRevoke
            ) = 0;

        //
        // The Reference routines are called to obtain the
        // object that a handle refers to. The caller must
        // specify the rights that the handle must hold for
        // the operation that it is about to perform. The caller
        // is responsible for converting generic rights to specific
        // rights. The caller must also specify what object types
        // are permissible for the object.
        //
        // The returned object[s], on success, are referenced,
        // and the caller is responsible for releasing those references
        // when appropriate.
        //

        virtual
        PAL_ERROR
        ReferenceObjectByHandle(
            CPalThread *pThread,                // IN, OPTIONAL
            HANDLE hHandleToReference,
            CAllowedObjectTypes *pAllowedTypes,
            IPalObject **ppObject               // OUT
            ) = 0;

        //
        // This routine is intended for WaitForMultipleObjects[Ex]
        //

        virtual
        PAL_ERROR
        ReferenceMultipleObjectsByHandleArray(
            CPalThread *pThread,                // IN, OPTIONAL
            HANDLE rghHandlesToReference[],
            DWORD dwHandleCount,
            CAllowedObjectTypes *pAllowedTypes,
            IPalObject *rgpObjects[]            // OUT
            ) = 0;
    };

    extern IPalObjectManager *g_pObjectManager;

    enum ThreadWakeupReason
    {
        WaitSucceeded,
        Alerted,
        MutexAbandoned,
        WaitTimeout,
        WaitFailed
    };

    class IPalSynchronizationManager
    {
    public:

        //
        // A thread calls BlockThread to put itself to sleep after it has
        // registered itself with the objects it is to wait on. A thread
        // need not have registered with any objects, as would occur in
        // the implementation of Sleep[Ex].
        //
        // Needless to say a thread must not be holding any PAL locks
        // directly or implicitly (e.g., by holding a reference to a
        // synchronization controller) when it calls this method.
        //

        virtual
        PAL_ERROR
        BlockThread(
            CPalThread *pCurrentThread,
            DWORD dwTimeout,
            bool fAlertable,
            bool fIsSleep,
            ThreadWakeupReason *peWakeupReason, // OUT
            DWORD *pdwSignaledObject       // OUT
            ) = 0;

        virtual
        PAL_ERROR
        AbandonObjectsOwnedByThread(
            CPalThread *pCallingThread,
            CPalThread *pTargetThread
            ) = 0;

        virtual
        PAL_ERROR
        QueueUserAPC(
            CPalThread *pThread,
            CPalThread *pTargetThread,
            PAPCFUNC pfnAPC,
            ULONG_PTR dwData
            ) = 0;

        virtual
        bool
        AreAPCsPending(
            CPalThread *pThread
            ) = 0;

        virtual
        PAL_ERROR
        DispatchPendingAPCs(
            CPalThread *pThread
            ) = 0;

        //
        // This routine is primarily meant for use by WaitForMultipleObjects[Ex].
        // The caller must individually release each of the returned controller
        // interfaces.
        //

        virtual
        PAL_ERROR
        GetSynchWaitControllersForObjects(
            CPalThread *pThread,
            IPalObject *rgObjects[],
            DWORD dwObjectCount,
            ISynchWaitController *rgControllers[]
            ) = 0;

        virtual
        PAL_ERROR
        GetSynchStateControllersForObjects(
            CPalThread *pThread,
            IPalObject *rgObjects[],
            DWORD dwObjectCount,
            ISynchStateController *rgControllers[]
            ) = 0;

        //
        // These following routines are meant for use only by IPalObject
        // implementations. The first two routines are used to
        // allocate and free an object's synchronization state; the third
        // is called during object promotion.
        //

        virtual
        PAL_ERROR
        AllocateObjectSynchData(
            CObjectType *pObjectType,
            VOID **ppvSynchData                 // OUT
            ) = 0;

        virtual
        void
        FreeObjectSynchData(
            CObjectType *pObjectType,
            VOID *pvSynchData
            ) = 0;

        //
        // The next two routines provide access to the process-wide
        // synchronization lock
        //

        virtual
        void
        AcquireProcessLock(
            CPalThread *pThread
            ) = 0;

        virtual
        void
        ReleaseProcessLock(
            CPalThread *pThread
            ) = 0;

        //
        // The final routines are used by IPalObject::GetSynchStateController
        // and IPalObject::GetSynchWaitController
        //

        virtual
        PAL_ERROR
        CreateSynchStateController(
            CPalThread *pThread,                // IN, OPTIONAL
            CObjectType *pObjectType,
            VOID *pvSynchData,
            ISynchStateController **ppStateController       // OUT
            ) = 0;

        virtual
        PAL_ERROR
        CreateSynchWaitController(
            CPalThread *pThread,                // IN, OPTIONAL
            CObjectType *pObjectType,
            VOID *pvSynchData,
            ISynchWaitController **ppWaitController       // OUT
            ) = 0;
    };

    extern IPalSynchronizationManager *g_pSynchronizationManager;

}

#endif // _CORUNIX_H

