// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.


#include "common.h"

#include "ecall.h"
#include "eetwain.h"
#include "dbginterface.h"
#include "gcenv.h"

#ifdef USE_GC_INFO_DECODER
#include "gcinfodecoder.h"
#endif

#ifdef HAVE_GCCOVER
#include "gccover.h"
#endif // HAVE_GCCOVER

#ifdef FEATURE_INTERPRETER
#include "interpexec.h"
#endif // FEATURE_INTERPRETER

#include "exinfo.h"
#include "excep.h"

#ifdef TARGET_X86

// NOTE: enabling compiler optimizations, even for debug builds.
// Comment this out in order to be able to fully debug methods here.
#if defined(_MSC_VER)
#pragma optimize("tg", on)
#endif

void promoteVarArgs(PTR_BYTE argsStart, PTR_VASigCookie varArgSig, GCCONTEXT* ctx);

#include "gc_unwind_x86.inl"

#endif // TARGET_X86

#include "argdestination.h"

#include "exceptionhandling.h"

#ifndef DACCESS_COMPILE
#ifndef FEATURE_EH_FUNCLETS

/*****************************************************************************
 *
 *  Setup context to enter an exception handler (a 'catch' block).
 *  This is the last chance for the runtime support to do fixups in
 *  the context before execution continues inside a filter, catch handler,
 *  or finally.
 */
void EECodeManager::FixContext( ContextType     ctxType,
                                EHContext      *ctx,
                                EECodeInfo     *pCodeInfo,
                                DWORD           dwRelOffset,
                                DWORD           nestingLevel,
                                OBJECTREF       thrownObject,
                                size_t       ** ppShadowSP,
                                size_t       ** ppEndRegion)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    _ASSERTE((ctxType == FINALLY_CONTEXT) == (thrownObject == NULL));

    /* Extract the necessary information from the info block header */
    hdrInfo hdrInfoBody = { 0 };
    pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody, dwRelOffset);

#ifdef  _DEBUG
    if (trFixContext) {
        minipal_log_print_info("FixContext [%s][%s] for %s.%s: ",
               hdrInfoBody.ebpFrame?"ebp":"   ",
               hdrInfoBody.interruptible?"int":"   ",
               "UnknownClass","UnknownMethod");
        minipal_log_flush_info();
    }
#endif

    /* make sure that we have an ebp stack frame */

    _ASSERTE(hdrInfoBody.ebpFrame);
    _ASSERTE(hdrInfoBody.handlers); // <TODO>@TODO : This will always be set. Remove it</TODO>

    TADDR      baseSP;
    GetHandlerFrameInfo(&hdrInfoBody, ctx->Ebp,
                                ctxType == FILTER_CONTEXT ? ctx->Esp : IGNORE_VAL,
                                ctxType == FILTER_CONTEXT ? (DWORD) IGNORE_VAL : nestingLevel,
                                &baseSP,
                                &nestingLevel);

    _ASSERTE((size_t)ctx->Ebp >= baseSP);
    _ASSERTE(baseSP >= (size_t)ctx->Esp);

    ctx->Esp = (DWORD)baseSP;

    // EE will write Esp to **pShadowSP before jumping to handler

    PTR_TADDR pBaseSPslots =
        GetFirstBaseSPslotPtr(ctx->Ebp, &hdrInfoBody);
    *ppShadowSP = (size_t *)&pBaseSPslots[-(int) nestingLevel   ];
                   pBaseSPslots[-(int)(nestingLevel+1)] = 0; // Zero out the next slot

    // EE will write the end offset of the filter
    if (ctxType == FILTER_CONTEXT)
        *ppEndRegion = (size_t *)pBaseSPslots + 1;

    /*  This is just a simple assignment of throwObject to ctx->Eax,
        just pretend the cast goo isn't there.
     */

    *((OBJECTREF*)&(ctx->Eax)) = thrownObject;
}

#endif // !FEATURE_EH_FUNCLETS





/*****************************************************************************/

bool        VarIsInReg(ICorDebugInfo::VarLoc varLoc)
{
    LIMITED_METHOD_CONTRACT;

    switch(varLoc.vlType)
    {
    case ICorDebugInfo::VLT_REG:
    case ICorDebugInfo::VLT_REG_REG:
    case ICorDebugInfo::VLT_REG_STK:
        return true;

    default:
        return false;
    }
}

#ifdef FEATURE_REMAP_FUNCTION
/*****************************************************************************
 *  Last chance for the runtime support to do fixups in the context
 *  before execution continues inside an EnC updated function.
 *  It also adjusts ESP and munges on the stack. So the caller has to make
 *  sure that this stack region is not needed (by doing a localloc).
 *  Also, if this returns EnC_FAIL, we should not have munged the
 *  context ie. transcated commit
 *  The plan of attack is:
 *  1) Error checking up front.  If we get through here, everything
 *      else should work
 *  2) Get all the info about current variables, registers, etc
 *  3) zero out the stack frame - this'll initialize _all_ variables
 *  4) Put the variables from step 3 into their new locations.
 *
 *  Note that while we use the ShuffleVariablesGet/Set methods, they don't
 *  have any info/logic that's internal to the runtime: another codemanger
 *  could easily duplicate what they do, which is why we're calling into them.
 */

 HRESULT EECodeManager::FixContextForEnC(PCONTEXT         pCtx,
                                        EECodeInfo *     pOldCodeInfo,
                   const ICorDebugInfo::NativeVarInfo *  oldMethodVars,
                                        SIZE_T           oldMethodVarsCount,
                                        EECodeInfo *     pNewCodeInfo,
                   const ICorDebugInfo::NativeVarInfo *  newMethodVars,
                                        SIZE_T           newMethodVarsCount)
{
    CONTRACTL {
        DISABLED(NOTHROW);
        DISABLED(GC_NOTRIGGER);
    } CONTRACTL_END;

    HRESULT hr = S_OK;

     // Grab a copy of the context before the EnC update.
    T_CONTEXT oldCtx = *pCtx;

#if defined(TARGET_X86)

    /* Extract the necessary information from the info block header */

    hdrInfo  *oldInfo, *newInfo;

    pOldCodeInfo->DecodeGCHdrInfo(&oldInfo);
    pNewCodeInfo->DecodeGCHdrInfo(&newInfo);

    //1) Error checking up front.  If we get through here, everything
    //     else should work

    if (!oldInfo->editNcontinue || !newInfo->editNcontinue) {
        LOG((LF_ENC, LL_INFO100, "**Error** EECM::FixContextForEnC EnC_INFOLESS_METHOD\n"));
        return CORDBG_E_ENC_INFOLESS_METHOD;
    }

    if (!oldInfo->ebpFrame || !newInfo->ebpFrame) {
        LOG((LF_ENC, LL_INFO100, "**Error** EECM::FixContextForEnC Esp frames NYI\n"));
        return E_FAIL; // Esp frames NYI
    }

    if (pCtx->Esp != pCtx->Ebp - oldInfo->stackSize + sizeof(DWORD)) {
        LOG((LF_ENC, LL_INFO100, "**Error** EECM::FixContextForEnC stack should be empty\n"));
        return E_FAIL; // stack should be empty - <TODO> @TODO : Barring localloc</TODO>
    }

#ifdef FEATURE_EH_FUNCLETS
    // EnC remap inside handlers is not supported
    if (pOldCodeInfo->IsFunclet() || pNewCodeInfo->IsFunclet())
        return CORDBG_E_ENC_IN_FUNCLET;
#else
    if (oldInfo->handlers)
    {
        bool      hasInnerFilter;
        TADDR     baseSP;
        FrameType frameType = GetHandlerFrameInfo(oldInfo, pCtx->Ebp,
                                                  pCtx->Esp, IGNORE_VAL,
                                                  &baseSP, NULL, &hasInnerFilter);
        _ASSERTE(frameType != FR_INVALID);
        _ASSERTE(!hasInnerFilter); // FixContextForEnC() is called for bottommost funclet

        // If the method is in a fuclet, and if the framesize grows, we are in trouble.

        if (frameType != FR_NORMAL)
        {
           /* <TODO> @TODO : What if the new method offset is in a fuclet,
              and the old is not, or the nesting level changed, etc </TODO> */

            if (oldInfo->stackSize != newInfo->stackSize) {
                LOG((LF_ENC, LL_INFO100, "**Error** EECM::FixContextForEnC stack size mismatch\n"));
                return CORDBG_E_ENC_IN_FUNCLET;
            }
        }
    }
#endif // FEATURE_EH_FUNCLETS

    /* @TODO: Check if we have grown out of space for locals, in the face of localloc */
    _ASSERTE(!oldInfo->localloc && !newInfo->localloc);

#ifndef FEATURE_EH_FUNCLETS
    // @TODO: If nesting level grows above the MAX_EnC_HANDLER_NESTING_LEVEL,
    // we should return EnC_NESTED_HANLDERS
    _ASSERTE(oldInfo->handlers && newInfo->handlers);
#endif

    LOG((LF_ENC, LL_INFO100, "EECM::FixContextForEnC: Checks out\n"));

#elif defined(TARGET_AMD64) || defined(TARGET_ARM64)

    // Strategy for zeroing out the frame on x64:
    //
    // The stack frame looks like this (stack grows up)
    //
    // =======================================
    //             <--- RSP == RBP (invariant: localalloc disallowed before remap)
    // Arguments for next call (if there is one)
    // JIT temporaries (if any)
    // Security object (if any)
    // Local variables (if any)
    // ---------------------------------------
    // Frame header (stuff we must preserve, such as bool for synchronized
    // methods, saved FP, saved callee-preserved registers, etc.)
    // Return address (also included in frame header)
    // ---------------------------------------
    // Arguments for this frame (that's getting remapped).  Will naturally be preserved
    // since fixed-frame size doesn't include this.
    // =======================================
    //
    // Goal: Zero out everything AFTER (above) frame header.
    //
    // How do we find this stuff?
    //
    // EECodeInfo::GetFixedStackSize() gives us the full size from the top ("Arguments
    // for next call") all the way down to and including Return Address.
    //
    // GetSizeOfEditAndContinuePreservedArea() gives us the size in bytes of the
    // frame header at the bottom.
    //
    // So we start at RSP, and zero out:
    //     GetFixedStackSize() - GetSizeOfEditAndContinuePreservedArea() bytes.
    //
    // We'll need to copy security object; location gotten from GCInfo.
    //
    // On ARM64 the JIT generates a slightly different frame and we do not have
    // the invariant FP == SP, since the FP needs to point at the saved fp/lr
    // pair for ETW stack walks. The frame there looks something like:
    // =======================================
    // Arguments for next call (if there is one)     <- SP
    // JIT temporaries
    // Locals
    // ---------------------------------------    ^ zeroed area
    // MonitorAcquired (for synchronized methods)
    // Saved FP                                      <- FP
    // Saved LR
    // ---------------------------------------    ^ preserved area
    // Arguments
    //
    // The JIT reports the size of the "preserved" area, which includes
    // MonitorAcquired when it is present. It could also include other local
    // values that need to be preserved across EnC transitions, but no explicit
    // treatment of these is necessary here beyond preserving the values in
    // this region.

    // GCInfo for old method
    GcInfoDecoder oldGcDecoder(
        pOldCodeInfo->GetGCInfoToken(),
        GcInfoDecoderFlags(DECODE_SECURITY_OBJECT | DECODE_EDIT_AND_CONTINUE),
        0       // Instruction offset (not needed)
        );

    // GCInfo for new method
    GcInfoDecoder newGcDecoder(
        pNewCodeInfo->GetGCInfoToken(),
        GcInfoDecoderFlags(DECODE_SECURITY_OBJECT | DECODE_EDIT_AND_CONTINUE),
        0       // Instruction offset (not needed)
        );

    UINT32 oldSizeOfPreservedArea = oldGcDecoder.GetSizeOfEditAndContinuePreservedArea();
    UINT32 newSizeOfPreservedArea = newGcDecoder.GetSizeOfEditAndContinuePreservedArea();

    LOG((LF_CORDB, LL_INFO100, "EECM::FixContextForEnC: Got old and new EnC preserved area sizes of %u and %u\n", oldSizeOfPreservedArea, newSizeOfPreservedArea));
    // This ensures the JIT generated EnC compliant code.
    if ((oldSizeOfPreservedArea == NO_SIZE_OF_EDIT_AND_CONTINUE_PRESERVED_AREA) ||
        (newSizeOfPreservedArea == NO_SIZE_OF_EDIT_AND_CONTINUE_PRESERVED_AREA))
    {
        _ASSERTE(!"FixContextForEnC called on a non-EnC-compliant method frame");
        return CORDBG_E_ENC_INFOLESS_METHOD;
    }

    TADDR oldStackBase = GetSP(&oldCtx);

    LOG((LF_CORDB, LL_INFO100, "EECM::FixContextForEnC: Old SP=%p, FP=%p\n", (void*)oldStackBase, (void*)GetFP(&oldCtx)));

#if defined(TARGET_AMD64)
    // Note: we cannot assert anything about the relationship between oldFixedStackSize
    // and newFixedStackSize.  It's possible the edited frame grows (new locals) or
    // shrinks (less temporaries).
    DWORD oldFixedStackSize = pOldCodeInfo->GetFixedStackSize();
    DWORD newFixedStackSize = pNewCodeInfo->GetFixedStackSize();

    // This verifies no localallocs were used in the old method.
    // JIT is required to emit frame register for EnC-compliant code
    _ASSERTE(pOldCodeInfo->HasFrameRegister());
    _ASSERTE(pNewCodeInfo->HasFrameRegister());

#elif defined(TARGET_ARM64)
    DWORD oldFixedStackSize = oldGcDecoder.GetSizeOfEditAndContinueFixedStackFrame();
    DWORD newFixedStackSize = newGcDecoder.GetSizeOfEditAndContinueFixedStackFrame();
#else
    PORTABILITY_ASSERT("Edit-and-continue not enabled on this platform.");
#endif

    LOG((LF_CORDB, LL_INFO100, "EECM::FixContextForEnC: Old and new fixed stack sizes are %u and %u\n", oldFixedStackSize, newFixedStackSize));

#if defined(TARGET_AMD64) && defined(TARGET_WINDOWS)
    // win-x64: SP == FP before localloc
    if (oldStackBase != GetFP(&oldCtx))
    {
        return E_FAIL;
    }
#else
    // All other 64-bit targets use frame chaining with the FP stored right below the
    // return address (LR is always pushed on arm64). FP + 16 == SP + oldFixedStackSize
    // gives the caller's SP before stack alloc.
    if (GetFP(&oldCtx) + 16 != oldStackBase + oldFixedStackSize)
    {
        return E_FAIL;
    }
#endif

    // EnC remap inside handlers is not supported
    if (pOldCodeInfo->IsFunclet() || pNewCodeInfo->IsFunclet())
        return CORDBG_E_ENC_IN_FUNCLET;

    if (oldSizeOfPreservedArea != newSizeOfPreservedArea)
    {
        _ASSERTE(!"FixContextForEnC called with method whose frame header size changed from old to new version.");
        return E_FAIL;
    }

    TADDR callerSP = oldStackBase + oldFixedStackSize;

#else
    PORTABILITY_ASSERT("Edit-and-continue not enabled on this platform.");
#endif

    // 2) Get all the info about current variables, registers, etc

    const ICorDebugInfo::NativeVarInfo *  pOldVar;

    // sorted by varNumber
    ICorDebugInfo::NativeVarInfo * oldMethodVarsSorted = NULL;
    ICorDebugInfo::NativeVarInfo * oldMethodVarsSortedBase = NULL;
    ICorDebugInfo::NativeVarInfo *newMethodVarsSorted = NULL;
    ICorDebugInfo::NativeVarInfo *newMethodVarsSortedBase = NULL;

    SIZE_T *rgVal1 = NULL;
    SIZE_T *rgVal2 = NULL;

    {
        SIZE_T local;

        // We'll need to sort the old native var info by variable number, since the
        // order of them isn't necc. the same.  We'll use the number as the key.
        // We will assume we may have hidden arguments (which have negative values as the index)

        unsigned oldNumVars = unsigned(-ICorDebugInfo::UNKNOWN_ILNUM);
        for (pOldVar = oldMethodVars, local = 0;
             local < oldMethodVarsCount;
             local++, pOldVar++)
        {
            DWORD varNumber = pOldVar->varNumber;
            if (signed(varNumber) >= 0)
            {
                // This is an explicit (not special) var, so add its varNumber + 1 to our
                // max count ("+1" because varNumber is zero-based).
                oldNumVars = max(oldNumVars, (unsigned)(unsigned(-ICorDebugInfo::UNKNOWN_ILNUM) + varNumber + 1));
            }
        }

        oldMethodVarsSortedBase = new (nothrow) ICorDebugInfo::NativeVarInfo[oldNumVars];
        if (!oldMethodVarsSortedBase)
        {
            hr = E_FAIL;
            goto ErrExit;
        }
        oldMethodVarsSorted = oldMethodVarsSortedBase + (-ICorDebugInfo::UNKNOWN_ILNUM);

        memset((void *)oldMethodVarsSortedBase, 0, oldNumVars * sizeof(ICorDebugInfo::NativeVarInfo));

        for (local = 0; local < oldNumVars;local++)
             oldMethodVarsSortedBase[local].loc.vlType = ICorDebugInfo::VLT_INVALID;

        BYTE **rgVCs = NULL;
        DWORD oldMethodOffset = pOldCodeInfo->GetRelOffset();

        for (pOldVar = oldMethodVars, local = 0;
             local < oldMethodVarsCount;
             local++, pOldVar++)
        {
            DWORD varNumber = pOldVar->varNumber;

            _ASSERTE(varNumber + unsigned(-ICorDebugInfo::UNKNOWN_ILNUM) < oldNumVars);

            // Only care about old local variables alive at oldMethodOffset
            if (pOldVar->startOffset <= oldMethodOffset &&
                pOldVar->endOffset   >  oldMethodOffset)
            {
                // Indexing should be performed with a signed value - could be negative.
                oldMethodVarsSorted[(int32_t)varNumber] = *pOldVar;
            }
        }

        // 3) Next sort the new var info by varNumber.  We want to do this here, since
        // we're allocating memory (which may fail) - do this before going to step 2

        // First, count the new vars the same way we did the old vars above.

        const ICorDebugInfo::NativeVarInfo * pNewVar;

        unsigned newNumVars = unsigned(-ICorDebugInfo::UNKNOWN_ILNUM);
        for (pNewVar = newMethodVars, local = 0;
             local < newMethodVarsCount;
             local++, pNewVar++)
        {
            DWORD varNumber = pNewVar->varNumber;
            if (signed(varNumber) >= 0)
            {
                // This is an explicit (not special) var, so add its varNumber + 1 to our
                // max count ("+1" because varNumber is zero-based).
                newNumVars = max(newNumVars, (unsigned)(unsigned(-ICorDebugInfo::UNKNOWN_ILNUM) + varNumber + 1));
            }
        }

        // sorted by varNumber
        newMethodVarsSortedBase = new (nothrow) ICorDebugInfo::NativeVarInfo[newNumVars];
        if (!newMethodVarsSortedBase)
        {
            hr = E_FAIL;
            goto ErrExit;
        }
        newMethodVarsSorted = newMethodVarsSortedBase + (-ICorDebugInfo::UNKNOWN_ILNUM);

        memset(newMethodVarsSortedBase, 0, newNumVars * sizeof(ICorDebugInfo::NativeVarInfo));
        for (local = 0; local < newNumVars;local++)
             newMethodVarsSortedBase[local].loc.vlType = ICorDebugInfo::VLT_INVALID;

        DWORD newMethodOffset = pNewCodeInfo->GetRelOffset();

        for (pNewVar = newMethodVars, local = 0;
             local < newMethodVarsCount;
             local++, pNewVar++)
        {
            DWORD varNumber = pNewVar->varNumber;

            _ASSERTE(varNumber + unsigned(-ICorDebugInfo::UNKNOWN_ILNUM) < newNumVars);

            // Only care about new local variables alive at newMethodOffset
            if (pNewVar->startOffset <= newMethodOffset &&
                pNewVar->endOffset   >  newMethodOffset)
            {
                // Indexing should be performed with a signed valued - could be negative.
                newMethodVarsSorted[(int32_t)varNumber] = *pNewVar;
            }
        }

        _ASSERTE(newNumVars >= oldNumVars ||
                 !"Not allowed to reduce the number of locals between versions!");

        LOG((LF_ENC, LL_INFO100, "EECM::FixContextForEnC: gathered info!\n"));

        rgVal1 = new (nothrow) SIZE_T[newNumVars];
        if (rgVal1 == NULL)
        {
            hr = E_FAIL;
            goto ErrExit;
        }

        rgVal2 = new (nothrow) SIZE_T[newNumVars];
        if (rgVal2 == NULL)
        {
            hr = E_FAIL;
            goto ErrExit;
        }

        // 4) Next we'll zero them out, so any variables that aren't in scope
        // in the old method, but are in scope in the new, will have the
        // default, zero, value.

        memset(rgVal1, 0, sizeof(SIZE_T) * newNumVars);
        memset(rgVal2, 0, sizeof(SIZE_T) * newNumVars);

        unsigned varsToGet = (oldNumVars > newNumVars)
                ? newNumVars
                : oldNumVars;

         //  2) Get all the info about current variables, registers, etc.

        hr = g_pDebugInterface->GetVariablesFromOffset(pOldCodeInfo->GetMethodDesc(),
                                                       varsToGet,
                                                       oldMethodVarsSortedBase,
                                                       oldMethodOffset,
                                                       &oldCtx,
                                                       rgVal1,
                                                       rgVal2,
                                                       newNumVars,
                                                       &rgVCs);
        if (FAILED(hr))
        {
            goto ErrExit;
        }


        LOG((LF_ENC, LL_INFO100, "EECM::FixContextForEnC: got vars!\n"));

        /*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*
         *  IMPORTANT : Once we start munging on the context, we cannot return
         *  EnC_FAIL, as this should be a transacted commit,
         **=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*/

#if defined(TARGET_X86)
        // Zero out all  the registers as some may hold new variables.
        pCtx->Eax = pCtx->Ecx = pCtx->Edx = pCtx->Ebx = pCtx->Esi = pCtx->Edi = 0;

        // 3) zero out the stack frame - this'll initialize _all_ variables

        /*-------------------------------------------------------------------------
         * Adjust the stack height
         */
        pCtx->Esp -= (newInfo->stackSize - oldInfo->stackSize);

        // Zero-init the local and tempory section of new stack frame being careful to avoid
        // touching anything in the frame header.
        // This is necessary to ensure that any JIT temporaries in the old version can't be mistaken
        // for ObjRefs now.
        size_t frameHeaderSize = GetSizeOfFrameHeaderForEnC( newInfo );
        _ASSERTE( frameHeaderSize <= oldInfo->stackSize );
        _ASSERTE( GetSizeOfFrameHeaderForEnC( oldInfo ) == frameHeaderSize );

#elif defined(TARGET_AMD64) && !defined(UNIX_AMD64_ABI)

        // Next few statements zero out all registers that may end up holding new variables.

        // volatile int registers (JIT may use these to enregister variables)
        pCtx->Rax = pCtx->Rcx = pCtx->Rdx = pCtx->R8 = pCtx->R9 = pCtx->R10 = pCtx->R11 = 0;

        // volatile float registers
        pCtx->Xmm1.High = pCtx->Xmm1.Low = 0;
        pCtx->Xmm2.High = pCtx->Xmm2.Low = 0;
        pCtx->Xmm3.High = pCtx->Xmm3.Low = 0;
        pCtx->Xmm4.High = pCtx->Xmm4.Low = 0;
        pCtx->Xmm5.High = pCtx->Xmm5.Low = 0;

        // 3) zero out the stack frame - this'll initialize _all_ variables

        /*-------------------------------------------------------------------------
        * Adjust the stack height
        */

        TADDR newStackBase = callerSP - newFixedStackSize;

        SetSP(pCtx, newStackBase);

        // We want to zero-out everything pushed after the frame header. This way we'll zero
        // out locals (both old & new) and temporaries. This is necessary to ensure that any
        // JIT temporaries in the old version can't be mistaken for ObjRefs now. (I am told
        // this last point is less of an issue on x64 as it is on x86, but zeroing out the
        // temporaries is still the cleanest, most robust way to go.)
        size_t frameHeaderSize = newSizeOfPreservedArea;
        _ASSERTE(frameHeaderSize <= oldFixedStackSize);
        _ASSERTE(frameHeaderSize <= newFixedStackSize);

        // For EnC-compliant x64 code, FP == SP.  Since SP changed above, update FP now
        pCtx->Rbp = newStackBase;

#else
#if defined(TARGET_ARM64)
        // Zero out volatile part of stack frame
        // x0-x17
        memset(&pCtx->X[0], 0, sizeof(pCtx->X[0]) * 18);
        // v0-v7
        memset(&pCtx->V[0], 0, sizeof(pCtx->V[0]) * 8);
        // v16-v31
        memset(&pCtx->V[16], 0, sizeof(pCtx->V[0]) * 16);
#elif defined(TARGET_AMD64)
        // SysV ABI
        pCtx->Rax = pCtx->Rdi = pCtx->Rsi = pCtx->Rdx = pCtx->Rcx = pCtx->R8 = pCtx->R9 = 0;

        // volatile float registers
        memset(&pCtx->Xmm0, 0, sizeof(pCtx->Xmm0) * 16);
#else
        PORTABILITY_ASSERT("Edit-and-continue not enabled on this platform.");
#endif

        TADDR newStackBase = callerSP - newFixedStackSize;

        SetSP(pCtx, newStackBase);

        size_t frameHeaderSize = newSizeOfPreservedArea;
        _ASSERTE(frameHeaderSize <= oldFixedStackSize);
        _ASSERTE(frameHeaderSize <= newFixedStackSize);

        // EnC prolog saves only FP (and LR on arm64), and FP points to saved FP for frame chaining.
        // These should already be set up from previous version.
        _ASSERTE(GetFP(pCtx) == callerSP - 16);
#endif

        // Perform some debug-only sanity checks on stack variables.  Some checks are
        // performed differently between X86/AMD64.

#ifdef _DEBUG
        for( unsigned i = 0; i < newNumVars; i++ )
        {
            // Make sure that stack variables existing in both old and new methods did not
            // move.  This matters if the address of a local is used in the remapped method.
            // For example:
            //
            //    static unsafe void Main(string[] args)
            //    {
            //        int x;
            //        int* p = &x;
            //                 <- Edit made here - cannot move address of x
            //        *p = 5;
            //    }
            //
            if ((i + unsigned(-ICorDebugInfo::UNKNOWN_ILNUM) < oldNumVars) &&  // Does variable exist in old method?
                 (oldMethodVarsSorted[i].loc.vlType == ICorDebugInfo::VLT_STK) &&   // Is the variable on the stack?
                 (newMethodVarsSorted[i].loc.vlType == ICorDebugInfo::VLT_STK))
            {
                SIZE_T * pOldVarStackLocation = NativeVarStackAddr(oldMethodVarsSorted[i].loc, &oldCtx);
                SIZE_T * pNewVarStackLocation = NativeVarStackAddr(newMethodVarsSorted[i].loc, pCtx);
                _ASSERTE(pOldVarStackLocation == pNewVarStackLocation);
            }

            // Sanity-check that the range we're clearing contains all of the stack variables

#if defined(TARGET_X86)
            const ICorDebugInfo::VarLoc &varLoc = newMethodVarsSortedBase[i].loc;
            if( varLoc.vlType == ICorDebugInfo::VLT_STK )
            {
                // This is an EBP frame, all stack variables should be EBP relative
                _ASSERTE( varLoc.vlStk.vlsBaseReg == ICorDebugInfo::REGNUM_EBP );
                // Generic special args may show up as locals with positive offset from EBP, so skip them
                if( varLoc.vlStk.vlsOffset <= 0 )
                {
                    // Normal locals must occur after the header on the stack
                    _ASSERTE( unsigned(-varLoc.vlStk.vlsOffset) >= frameHeaderSize );
                    // Value must occur before the top of the stack
                    _ASSERTE( unsigned(-varLoc.vlStk.vlsOffset) < newInfo->stackSize );
                }

                // Ideally we'd like to verify that the stack locals (if any) start at exactly the end
                // of the header.  However, we can't easily determine the size of value classes here,
                // and so (since the stack grows towards 0) can't easily determine where the end of
                // the local lies.
            }
#elif defined(TARGET_AMD64) || defined(TARGET_ARM64)
            switch(newMethodVarsSortedBase[i].loc.vlType)
            {
            default:
                // No validation here for non-stack locals
                break;

            case ICorDebugInfo::VLT_STK_BYREF:
                {
                    // For byrefs, verify that the ptr will be zeroed out

                    SIZE_T regOffs = GetRegOffsInCONTEXT(newMethodVarsSortedBase[i].loc.vlStk.vlsBaseReg);
                    TADDR baseReg = *(TADDR *)(regOffs + (BYTE*)pCtx);
                    TADDR addrOfPtr = baseReg + newMethodVarsSortedBase[i].loc.vlStk.vlsOffset;

                    _ASSERTE(
                        // The ref must exist in the portion we'll zero-out
                        (
                            (newStackBase <= addrOfPtr) &&
                            (addrOfPtr < newStackBase + (newFixedStackSize - frameHeaderSize))
                        ) ||
                        // OR in the caller's frame (for parameters)
                        (addrOfPtr >= newStackBase + newFixedStackSize));

                    // Deliberately fall through, so that we also verify that the value that the ptr
                    // points to will be zeroed out
                    // ...
                }
                __fallthrough;

            case ICorDebugInfo::VLT_STK:
            case ICorDebugInfo::VLT_STK2:
            case ICorDebugInfo::VLT_REG_STK:
            case ICorDebugInfo::VLT_STK_REG:
                SIZE_T * pVarStackLocation = NativeVarStackAddr(newMethodVarsSortedBase[i].loc, pCtx);
                _ASSERTE (pVarStackLocation != NULL);
                _ASSERTE(
                    // The value must exist in the portion we'll zero-out
                    (
                        (newStackBase <= (TADDR) pVarStackLocation) &&
                        ((TADDR) pVarStackLocation < newStackBase + (newFixedStackSize - frameHeaderSize))
                    ) ||
                    // OR in the caller's frame (for parameters)
                    ((TADDR) pVarStackLocation >= newStackBase + newFixedStackSize));
                break;
            }
#else   // !X86, !X64, !ARM64
            PORTABILITY_ASSERT("Edit-and-continue not enabled on this platform.");
#endif
        }

#endif // _DEBUG

        // Clear the local and temporary stack space

#if defined(TARGET_X86)
        memset((void*)(size_t)(pCtx->Esp), 0, newInfo->stackSize - frameHeaderSize );
#elif defined(TARGET_AMD64) || defined(TARGET_ARM64)
        memset((void*)newStackBase, 0, newFixedStackSize - frameHeaderSize);
#else   // !X86, !X64, !ARM64
        PORTABILITY_ASSERT("Edit-and-continue not enabled on this platform.");
#endif

        // 4) Put the variables from step 3 into their new locations.

        LOG((LF_ENC, LL_INFO100, "EECM::FixContextForEnC: set vars!\n"));

        // Move the old variables into their new places.

        hr = g_pDebugInterface->SetVariablesAtOffset(pNewCodeInfo->GetMethodDesc(),
                                                     newNumVars,
                                                     newMethodVarsSortedBase,
                                                     newMethodOffset,
                                                     pCtx, // place them into the new context
                                                     rgVal1,
                                                     rgVal2,
                                                     rgVCs);

        /*-----------------------------------------------------------------------*/
    }
ErrExit:
    if (oldMethodVarsSortedBase)
        delete[] oldMethodVarsSortedBase;
    if (newMethodVarsSortedBase)
        delete[] newMethodVarsSortedBase;
    if (rgVal1 != NULL)
        delete[] rgVal1;
    if (rgVal2 != NULL)
        delete[] rgVal2;

    LOG((LF_ENC, LL_INFO100, "EECM::FixContextForEnC: exiting!\n"));

    return hr;
}
#endif // !FEATURE_METADATA_UPDATER

#endif // #ifndef DACCESS_COMPILE

#ifdef USE_GC_INFO_DECODER
/*****************************************************************************
 *
 *  Is the function currently at a "GC safe point" ?
 */
bool EECodeManager::IsGcSafe( EECodeInfo     *pCodeInfo,
                              DWORD           dwRelOffset)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

    GcInfoDecoder gcInfoDecoder(
            gcInfoToken,
            DECODE_INTERRUPTIBILITY,
            dwRelOffset
            );

    if (gcInfoDecoder.IsInterruptible())
        return true;

    if (gcInfoDecoder.IsSafePoint())
        return true;

    return false;
}

#if defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
bool EECodeManager::HasTailCalls( EECodeInfo     *pCodeInfo)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

    GcInfoDecoder gcInfoDecoder(
            gcInfoToken,
            DECODE_HAS_TAILCALLS,
            0
            );

    return gcInfoDecoder.HasTailCalls();
}
#endif // TARGET_ARM || TARGET_ARM64 || TARGET_LOONGARCH64 || TARGET_RISCV64

#else // !USE_GC_INFO_DECODER

/*****************************************************************************
 *
 *  Is the function currently at a "GC safe point" ?
 */
bool EECodeManager::IsGcSafe( EECodeInfo     *pCodeInfo,
                              DWORD           dwRelOffset)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    } CONTRACTL_END;

    hdrInfo info = { 0 };

    /* Extract the necessary information from the info block header */

    PTR_CBYTE table = pCodeInfo->DecodeGCHdrInfo(&info, dwRelOffset);

    /* workaround: prevent interruption within prolog/epilog */

    if  (info.prologOffs != hdrInfo::NOT_IN_PROLOG || info.epilogOffs != hdrInfo::NOT_IN_EPILOG)
        return false;

    if  (!info.interruptible)
        return false;

    return !::IsInNoGCRegion(&info, table, dwRelOffset);
}

#endif // !USE_GC_INFO_DECODER


#if defined(FEATURE_EH_FUNCLETS)

void EECodeManager::EnsureCallerContextIsValid( PREGDISPLAY  pRD, EECodeInfo * pCodeInfo /*= NULL*/, unsigned flags /*= 0*/)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    }
    CONTRACTL_END;

    if( !pRD->IsCallerContextValid )
    {
        if ((flags & LightUnwind) && (pCodeInfo != NULL))
        {
#if !defined(DACCESS_COMPILE) && defined(HAS_LIGHTUNWIND)
            LightUnwindStackFrame(pRD, pCodeInfo, EnsureCallerStackFrameIsValid);
#else
            // We need to make a copy here (instead of switching the pointers), in order to preserve the current context
            *(pRD->pCallerContext) = *(pRD->pCurrentContext);
            // Skip updating context registers for light unwind
            Thread::VirtualUnwindCallFrame(pRD->pCallerContext, NULL, pCodeInfo);
#endif
        }
        else
        {
            // We need to make a copy here (instead of switching the pointers), in order to preserve the current context
            *(pRD->pCallerContext) = *(pRD->pCurrentContext);
            *(pRD->pCallerContextPointers) = *(pRD->pCurrentContextPointers);
            Thread::VirtualUnwindCallFrame(pRD->pCallerContext, pRD->pCallerContextPointers, pCodeInfo);
        }

        pRD->IsCallerContextValid = TRUE;
    }

    _ASSERTE( pRD->IsCallerContextValid );
}

size_t EECodeManager::GetCallerSp( PREGDISPLAY  pRD )
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    } CONTRACTL_END;

    // Don't add usage of this field.  This is only temporary.
    // See ExInfo::InitializeCrawlFrame() for more information.
    if (!pRD->IsCallerSPValid)
    {
        ExecutionManager::GetDefaultCodeManager()->EnsureCallerContextIsValid(pRD, NULL);
    }

    return GetSP(pRD->pCallerContext);
}

#endif // FEATURE_EH_FUNCLETS

#ifdef HAS_LIGHTUNWIND
/*
  *  Light unwind the current stack frame, using provided cache entry.
  *  pPC, Esp and pEbp of pContext are updated.
  */

// static
void EECodeManager::LightUnwindStackFrame(PREGDISPLAY pRD, EECodeInfo* pCodeInfo, LightUnwindFlag flag)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

#ifdef TARGET_AMD64
    ULONG RBPOffset, RSPOffset;
    pCodeInfo->GetOffsetsFromUnwindInfo(&RSPOffset, &RBPOffset);

    if (pRD->IsCallerContextValid)
    {
        pRD->pCurrentContext->Rbp = pRD->pCallerContext->Rbp;
        pRD->pCurrentContext->Rsp = pRD->pCallerContext->Rsp;
        pRD->pCurrentContext->Rip = pRD->pCallerContext->Rip;
    }
    else
    {
        PCONTEXT pSourceCtx = NULL;
        PCONTEXT pTargetCtx = NULL;
        if (flag == UnwindCurrentStackFrame)
        {
            pTargetCtx = pRD->pCurrentContext;
            pSourceCtx = pRD->pCurrentContext;
        }
        else
        {
            pTargetCtx = pRD->pCallerContext;
            pSourceCtx = pRD->pCurrentContext;
        }

        // Unwind RBP.  The offset is relative to the current sp.
        if (RBPOffset == 0)
        {
            pTargetCtx->Rbp = pSourceCtx->Rbp;
        }
        else
        {
            pTargetCtx->Rbp = *(UINT_PTR*)(pSourceCtx->Rsp + RBPOffset);
        }

        // Adjust the sp.  From this pointer onwards pCurrentContext->Rsp is the caller sp.
        pTargetCtx->Rsp = pSourceCtx->Rsp + RSPOffset;

        // Retrieve the return address.
        pTargetCtx->Rip = *(UINT_PTR*)((pTargetCtx->Rsp) - sizeof(UINT_PTR));
    }

    if (flag == UnwindCurrentStackFrame)
    {
        SyncRegDisplayToCurrentContext(pRD);
        pRD->IsCallerContextValid = FALSE;
        pRD->IsCallerSPValid      = FALSE;        // Don't add usage of this field.  This is only temporary.
    }
#else
    PORTABILITY_ASSERT("EECodeManager::LightUnwindStackFrame is not implemented on this platform.");
#endif
}
#endif // HAS_LIGHTUNWIND

#ifdef FEATURE_EH_FUNCLETS
#ifdef TARGET_X86
size_t EECodeManager::GetResumeSp( PCONTEXT  pContext )
{
    PCODE currentPc = PCODE(pContext->Eip);

    _ASSERTE(ExecutionManager::IsManagedCode(currentPc));

    EECodeInfo codeInfo(currentPc);

    PTR_CBYTE methodStart = PTR_CBYTE(codeInfo.GetSavedMethodCode());

    DWORD       curOffs = codeInfo.GetRelOffset();

    hdrInfo    *hdrInfoBody;
    PTR_CBYTE   table = codeInfo.DecodeGCHdrInfo(&hdrInfoBody);

    _ASSERTE(hdrInfoBody->epilogOffs == hdrInfo::NOT_IN_EPILOG && hdrInfoBody->prologOffs == hdrInfo::NOT_IN_PROLOG);

    bool isESPFrame = !hdrInfoBody->ebpFrame && !hdrInfoBody->doubleAlign;

    if (codeInfo.IsFunclet())
    {
        // Treat funclet's frame as ESP frame
        isESPFrame = true;
    }

    if (isESPFrame)
    {
        const size_t curESP = (size_t)(pContext->Esp);
        return curESP + GetPushedArgSize(hdrInfoBody, table, curOffs);
    }

    const size_t curEBP = (size_t)(pContext->Ebp);
    return GetOutermostBaseFP(curEBP, hdrInfoBody);
}
#endif // TARGET_X86
#endif // FEATURE_EH_FUNCLETS

#ifndef FEATURE_EH_FUNCLETS

/*****************************************************************************
 *
 *  Unwind the current stack frame, i.e. update the virtual register
 *  set in pRD. This will be similar to the state after the function
 *  returns back to caller (IP points to after the call, Frame and Stack
 *  pointer has been reset, callee-saved registers restored (if UpdateAllRegs),
 *  callee-unsaved registers are trashed.
 *  Returns success of operation.
 */

bool EECodeManager::UnwindStackFrame(PREGDISPLAY     pRD,
                                     EECodeInfo     *pCodeInfo,
                                     unsigned        flags)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    } CONTRACTL_END;

#ifdef TARGET_X86
    bool updateAllRegs = flags & UpdateAllRegs;

    // Address where the method has been interrupted
    PCODE       breakPC = pRD->ControlPC;
    _ASSERTE(PCODEToPINSTR(breakPC) == pCodeInfo->GetCodeAddress());

    hdrInfo    *hdrInfoBody;
    PTR_CBYTE   table = pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody);

    hdrInfoBody->isSpeculativeStackWalk = ((flags & SpeculativeStackwalk) != 0);

    return UnwindStackFrameX86(pRD,
                               PTR_CBYTE(pCodeInfo->GetSavedMethodCode()),
                               pCodeInfo->GetRelOffset(),
                               hdrInfoBody,
                               table,
                               IN_EH_FUNCLETS_COMMA(PTR_CBYTE(pCodeInfo->GetJitManager()->GetFuncletStartAddress(pCodeInfo)))
                               IN_EH_FUNCLETS_COMMA(pCodeInfo->IsFunclet())
                               updateAllRegs);
#else // TARGET_X86
    PORTABILITY_ASSERT("EECodeManager::UnwindStackFrame");
    return false;
#endif // _TARGET_???_
}

/*****************************************************************************/
#else // !FEATURE_EH_FUNCLETS
/*****************************************************************************/

bool EECodeManager::UnwindStackFrame(PREGDISPLAY     pRD,
                                     EECodeInfo     *pCodeInfo,
                                     unsigned        flags)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    _ASSERTE(pCodeInfo != NULL);

#ifdef HAS_LIGHTUNWIND
    if (flags & LightUnwind)
    {
        LightUnwindStackFrame(pRD, pCodeInfo, UnwindCurrentStackFrame);
        return true;
    }
#endif

    Thread::VirtualUnwindCallFrame(pRD, pCodeInfo);
    return true;
}

void EECodeManager::UnwindStackFrame(T_CONTEXT  *pContext)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    EECodeInfo codeInfo(dac_cast<PCODE>(GetIP(pContext)));
    Thread::VirtualUnwindCallFrame(pContext, NULL, &codeInfo);
}

/*****************************************************************************/
#endif // FEATURE_EH_FUNCLETS

/*****************************************************************************/

/* report args in 'msig' to the GC.
   'argsStart' is start of the stack-based arguments
   'varArgSig' describes the arguments
   'ctx' has the GC reporting info
*/
void promoteVarArgs(PTR_BYTE argsStart, PTR_VASigCookie varArgSig, GCCONTEXT* ctx)
{
    WRAPPER_NO_CONTRACT;

    SigTypeContext typeContext(varArgSig->classInst, varArgSig->methodInst);
    MetaSig msig(varArgSig->signature,
                 varArgSig->pModule,
                 &typeContext);

    PTR_BYTE pFrameBase = argsStart - TransitionBlock::GetOffsetOfArgs();

    ArgIterator argit(&msig);

#ifdef TARGET_X86
    // For the X86 target the JIT does not report any of the fixed args for a varargs method
    // So we report the fixed args via the promoteArgs call below
    bool skipFixedArgs = false;
#else
    // For other platforms the JITs do report the fixed args of a varargs method
    // So we must tell promoteArgs to skip to the end of the fixed args
    bool skipFixedArgs = true;
#endif

    bool inVarArgs = false;

    int argOffset;
    while ((argOffset = argit.GetNextOffset()) != TransitionBlock::InvalidOffset)
    {
        if (msig.GetArgProps().AtSentinel())
            inVarArgs = true;

        // if skipFixedArgs is false we report all arguments
        //  otherwise we just report the varargs.
        if (!skipFixedArgs || inVarArgs)
        {
            ArgDestination argDest(pFrameBase, argOffset, argit.GetArgLocDescForStructInRegs());
            msig.GcScanRoots(&argDest, ctx->f, ctx->sc);
        }
    }
}

#ifndef DACCESS_COMPILE
FCIMPL1(void, GCReporting::Register, GCFrame* frame)
{
    FCALL_CONTRACT;

    // Construct a GCFrame.
    _ASSERTE(frame != NULL);
    frame->Push(GetThread());
}
FCIMPLEND

FCIMPL1(void, GCReporting::Unregister, GCFrame* frame)
{
    FCALL_CONTRACT;

    // Destroy the GCFrame.
    _ASSERTE(frame != NULL);
    frame->Remove();
}
FCIMPLEND
#endif // !DACCESS_COMPILE

#ifndef USE_GC_INFO_DECODER

/*****************************************************************************
 *
 *  Enumerate all live object references in that function using
 *  the virtual register set.
 *  Returns success of operation.
 */

bool EECodeManager::EnumGcRefs( PREGDISPLAY     pContext,
                                EECodeInfo     *pCodeInfo,
                                unsigned        flags,
                                GCEnumCallback  pCallBack,
                                LPVOID          hCallBack,
                                DWORD           relOffsetOverride)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    PTR_CBYTE methodStart = PTR_CBYTE(pCodeInfo->GetSavedMethodCode());
    unsigned  curOffs = pCodeInfo->GetRelOffset();
    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

    if (relOffsetOverride != NO_OVERRIDE_OFFSET)
    {
        curOffs = relOffsetOverride;
    }

    return ::EnumGcRefsX86(pContext,
                           methodStart,
                           curOffs,
                           gcInfoToken,
                           IN_EH_FUNCLETS_COMMA(PTR_CBYTE(pCodeInfo->GetJitManager()->GetFuncletStartAddress(pCodeInfo)))
                           IN_EH_FUNCLETS_COMMA(pCodeInfo->IsFunclet())
                           IN_EH_FUNCLETS_COMMA(pCodeInfo->GetJitManager()->IsFilterFunclet(pCodeInfo))
                           flags,
                           pCallBack,
                           hCallBack);
}

#else // !USE_GC_INFO_DECODER


/*****************************************************************************
 *
 *  Enumerate all live object references in that function using
 *  the virtual register set.
 *  Returns success of operation.
 */

bool EECodeManager::EnumGcRefs( PREGDISPLAY     pRD,
                                EECodeInfo     *pCodeInfo,
                                unsigned        flags,
                                GCEnumCallback  pCallBack,
                                LPVOID          hCallBack,
                                DWORD           relOffsetOverride)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    unsigned curOffs = pCodeInfo->GetRelOffset();

#ifdef TARGET_ARM
    // On ARM, the low-order bit of an instruction pointer indicates Thumb vs. ARM mode.
    // Mask this off; all instructions are two-byte aligned.
    curOffs &= (~THUMB_CODE);
#endif // TARGET_ARM

#ifdef _DEBUG
    // Get the name of the current method
    const char * methodName = pCodeInfo->GetMethodDesc()->GetName();
    LOG((LF_GCINFO, LL_INFO1000, "Reporting GC refs for %s at offset %04x.\n",
        methodName, curOffs));
#endif

    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

#ifdef _DEBUG
    if (flags & ActiveStackFrame)
    {
        GcInfoDecoder _gcInfoDecoder(
                            gcInfoToken,
                            DECODE_INTERRUPTIBILITY,
                            curOffs
                            );
        _ASSERTE(_gcInfoDecoder.IsInterruptible() || _gcInfoDecoder.CouldBeSafePoint());
    }
#endif

    // Check if we have been given an override value for relOffset
    if (relOffsetOverride != NO_OVERRIDE_OFFSET)
    {
        // We've been given an override offset for GC Info
        curOffs = relOffsetOverride;

#ifdef TARGET_ARM
        // On ARM, the low-order bit of an instruction pointer indicates Thumb vs. ARM mode.
        // Mask this off; all instructions are two-byte aligned.
        curOffs &= (~THUMB_CODE);
#endif // TARGET_ARM

        LOG((LF_GCINFO, LL_INFO1000, "Adjusted GC reporting offset to provided override offset. Now reporting GC refs for %s at offset %04x.\n",
            methodName, curOffs));
    }


#if defined(FEATURE_EH_FUNCLETS)   // funclets
    if (pCodeInfo->GetJitManager()->IsFilterFunclet(pCodeInfo))
    {
        // Filters are the only funclet that run during the 1st pass, and must have
        // both the leaf and the parent frame reported.  In order to avoid double
        // reporting of the untracked variables, do not report them for the filter.
        flags |= NoReportUntracked;
    }
#endif // FEATURE_EH_FUNCLETS

    bool reportScratchSlots;

    // We report scratch slots only for leaf frames.
    // A frame is non-leaf if we are executing a call, or a fault occurred in the function.
    // The only case in which we need to report scratch slots for a non-leaf frame
    //   is when execution has to be resumed at the point of interruption (via ResumableFrame)
    _ASSERTE( sizeof( BOOL ) >= sizeof( ActiveStackFrame ) );
    reportScratchSlots = (flags & ActiveStackFrame) != 0;


    GcInfoDecoder gcInfoDecoder(
                        gcInfoToken,
                        GcInfoDecoderFlags (DECODE_GC_LIFETIMES | DECODE_SECURITY_OBJECT | DECODE_VARARG),
                        curOffs
                        );

    if (!gcInfoDecoder.EnumerateLiveSlots(
                        pRD,
                        reportScratchSlots,
                        flags,
                        pCallBack,
                        hCallBack
                        ))
    {
        return false;
    }

#ifdef FEATURE_EH_FUNCLETS   // funclets
    //
    // If we're in a funclet, we do not want to report the incoming varargs.  This is
    // taken care of by the parent method and the funclet should access those arguments
    // by way of the parent method's stack frame.
    //
    if(pCodeInfo->IsFunclet())
    {
        return true;
    }
#endif // FEATURE_EH_FUNCLETS

    if (gcInfoDecoder.GetIsVarArg())
    {
        MethodDesc* pMD = pCodeInfo->GetMethodDesc();
        _ASSERTE(pMD != NULL);

        // This does not apply to x86 because of how it handles varargs (it never
        // reports the arguments from the explicit method signature).
        //
#ifndef TARGET_X86
        //
        // SPECIAL CASE:
        //      IL marshaling stubs have signatures that are marked as vararg,
        //      but they are callsite sigs that actually contain complete sig
        //      info.  There are two reasons for this:
        //          1) the stub callsites expect the method to be vararg
        //          2) the marshaling stub must have full sig info so that
        //             it can do a ldarg.N on the arguments it needs to marshal.
        //      The result of this is that the code below will report the
        //      variable arguments twice--once from the va sig cookie and once
        //      from the explicit method signature (in the method's gc info).
        //
        //      This fix to this is to early out of the va sig cookie reporting
        //      in this special case.
        //
        if (pMD->IsILStub())
        {
            return true;
        }
#endif // !TARGET_X86

        LOG((LF_GCINFO, LL_INFO100, "Reporting incoming vararg GC refs\n"));

        // Find the offset of the VASigCookie.  It's offsets are relative to
        // the base of a FramedMethodFrame.
        int VASigCookieOffset;

        {
            MetaSig msigFindVASig(pMD);
            ArgIterator argit(&msigFindVASig);
            VASigCookieOffset = argit.GetVASigCookieOffset() - TransitionBlock::GetOffsetOfArgs();
        }

        PTR_BYTE prevSP = dac_cast<PTR_BYTE>(GetCallerSp(pRD));

        _ASSERTE(prevSP + VASigCookieOffset >= dac_cast<PTR_BYTE>(GetSP(pRD->pCurrentContext)));

#if defined(_DEBUG) && !defined(DACCESS_COMPILE)
        // Note that I really want to say hCallBack is a GCCONTEXT, but this is pretty close
        extern void GcEnumObject(LPVOID pData, OBJECTREF *pObj, uint32_t flags);
        _ASSERTE((void*) GcEnumObject == pCallBack);
#endif // _DEBUG && !DACCESS_COMPILE
        GCCONTEXT   *pCtx = (GCCONTEXT *) hCallBack;

        // For varargs, look up the signature using the varArgSig token passed on the stack
        PTR_VASigCookie varArgSig = *PTR_PTR_VASigCookie(prevSP + VASigCookieOffset);

        promoteVarArgs(prevSP, varArgSig, pCtx);
    }

    return true;

}

#endif // USE_GC_INFO_DECODER

/*****************************************************************************
 *
 *  Returns "this" pointer if it is a non-static method
 *  AND the object is still alive.
 *  Returns NULL in all other cases.
 *  Unfortunately, the semantics of this call currently depend on the architecture.
 *  On non-x86 architectures, where we use GcInfo{En,De}Coder, this returns NULL for
 *  all cases except the case where the GenericsContext is determined via "this."  On x86,
 *  it will definitely return a non-NULL value in that case, and for synchronized methods;
 *  it may also return a non-NULL value for other cases, depending on how the method is compiled.
 */
OBJECTREF EECodeManager::GetInstance( PREGDISPLAY    pContext,
                                      EECodeInfo*   pCodeInfo)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_COOPERATIVE;
        SUPPORTS_DAC;
    } CONTRACTL_END;

#ifndef USE_GC_INFO_DECODER
    unsigned    relOffset = pCodeInfo->GetRelOffset();

    PTR_CBYTE   table;
    hdrInfo    *hdrInfoBody;
    unsigned    stackDepth;
    TADDR       taArgBase;

    /* Extract the necessary information from the info block header */

    table = pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody);

    // We do not have accurate information in the prolog or the epilog
    if (hdrInfoBody->prologOffs != hdrInfo::NOT_IN_PROLOG ||
        hdrInfoBody->epilogOffs != hdrInfo::NOT_IN_EPILOG)
    {
        return NULL;
    }

    if  (hdrInfoBody->interruptible)
    {
        stackDepth = scanArgRegTableI(skipToArgReg(*hdrInfoBody, table), relOffset, relOffset, hdrInfoBody);
    }
    else
    {
        stackDepth = scanArgRegTable (skipToArgReg(*hdrInfoBody, table), (unsigned)relOffset, hdrInfoBody);
    }

    if (hdrInfoBody->ebpFrame)
    {
        _ASSERTE(stackDepth == 0);
        taArgBase = GetRegdisplayFP(pContext);
    }
    else
    {
        taArgBase =  pContext->SP + stackDepth;
    }

    // Only synchronized methods and generic code that accesses
    // the type context via "this" need to report "this".
    // If it's reported for other methods, it's probably
    // done incorrectly. So flag such cases.
    _ASSERTE(hdrInfoBody->thisPtrResult == REGI_NA ||
             pCodeInfo->GetMethodDesc()->IsSynchronized() ||
             pCodeInfo->GetMethodDesc()->AcquiresInstMethodTableFromThis());

    if (hdrInfoBody->thisPtrResult != REGI_NA)
    {
        // the register contains the Object pointer.
        TADDR uRegValue = *(reinterpret_cast<TADDR *>(getCalleeSavedReg(pContext, hdrInfoBody->thisPtrResult)));
        return ObjectToOBJECTREF(PTR_Object(uRegValue));
    }

#if VERIFY_GC_TABLES
    _ASSERTE(*castto(table, unsigned short *)++ == 0xBEEF);
#endif

    /* Skip over no-GC region table */
    unsigned count = hdrInfoBody->noGCRegionCnt;
    while (count-- > 0)
    {
        fastSkipUnsigned(table); fastSkipUnsigned(table);
    }

#ifndef FEATURE_EH_FUNCLETS
    /* Parse the untracked frame variable table */

    /* The 'this' pointer can never be located in the untracked table */
    /* as we only allow pinned and byrefs in the untracked table      */

    count = hdrInfoBody->untrackedCnt;
    while (count-- > 0)
    {
        fastSkipSigned(table);
    }

    /* Look for the 'this' pointer in the frame variable lifetime table     */

    count = hdrInfoBody->varPtrTableSize;
    unsigned tmpOffs = 0;
    while (count-- > 0)
    {
        unsigned varOfs = fastDecodeUnsigned(table);
        unsigned begOfs = tmpOffs + fastDecodeUnsigned(table);
        unsigned endOfs = begOfs + fastDecodeUnsigned(table);
        _ASSERTE(!hdrInfoBody->ebpFrame || (varOfs!=0));
        /* Is this variable live right now? */
        if (((unsigned)relOffset >= begOfs) && ((unsigned)relOffset < endOfs))
        {
            /* Does it contain the 'this' pointer */
            if (varOfs & this_OFFSET_FLAG)
            {
                unsigned ofs = varOfs & ~OFFSET_MASK;

                /* Tracked locals for EBP frames are always at negative offsets */

                if (hdrInfoBody->ebpFrame)
                    taArgBase -= ofs;
                else
                    taArgBase += ofs;

                return (OBJECTREF)(size_t)(*PTR_DWORD(taArgBase));
            }
        }
        tmpOffs = begOfs;
    }

#if VERIFY_GC_TABLES
    _ASSERTE(*castto(table, unsigned short *) == 0xBABE);
#endif

#else // FEATURE_EH_FUNCLETS
    if (pCodeInfo->GetMethodDesc()->AcquiresInstMethodTableFromThis() && (hdrInfoBody->genericsContext)) // Generic Context is "this"
    {
        // Untracked table must have at least one entry - this pointer
        _ASSERTE(hdrInfoBody->untrackedCnt > 0);

        // The first entry must be "this" pointer
        int stkOffs = fastDecodeSigned(table);
        taArgBase -= stkOffs & ~OFFSET_MASK;
        return (OBJECTREF)(size_t)(*PTR_DWORD(taArgBase));
    }
#endif // FEATURE_EH_FUNCLETS

    return NULL;
#else // !USE_GC_INFO_DECODER
    PTR_VOID token = EECodeManager::GetExactGenericsToken(pContext, pCodeInfo);

    OBJECTREF oRef = ObjectToOBJECTREF(PTR_Object(dac_cast<TADDR>(token)));
    VALIDATEOBJECTREF(oRef);
    return oRef;
#endif // USE_GC_INFO_DECODER
}

GenericParamContextType EECodeManager::GetParamContextType(PREGDISPLAY     pContext,
                                                           EECodeInfo *    pCodeInfo)
{
    LIMITED_METHOD_DAC_CONTRACT;

#ifndef USE_GC_INFO_DECODER
    /* Extract the necessary information from the info block header */
    unsigned    relOffset = pCodeInfo->GetRelOffset();

    hdrInfo    *hdrInfoBody;
    PTR_CBYTE   table = pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody);

    if (!hdrInfoBody->genericsContext ||
        hdrInfoBody->prologOffs != hdrInfo::NOT_IN_PROLOG ||
        hdrInfoBody->epilogOffs != hdrInfo::NOT_IN_EPILOG)
    {
        return GENERIC_PARAM_CONTEXT_NONE;
    }

    if (hdrInfoBody->genericsContextIsMethodDesc)
    {
        return GENERIC_PARAM_CONTEXT_METHODDESC;
    }

    return GENERIC_PARAM_CONTEXT_METHODTABLE;

    // On x86 the generic param context parameter is never this.
#else // !USE_GC_INFO_DECODER
    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

    GcInfoDecoder gcInfoDecoder(
            gcInfoToken,
            GcInfoDecoderFlags (DECODE_GENERICS_INST_CONTEXT)
            );

    INT32 spOffsetGenericsContext = gcInfoDecoder.GetGenericsInstContextStackSlot();
    if (spOffsetGenericsContext != NO_GENERICS_INST_CONTEXT)
    {
        if (gcInfoDecoder.HasMethodDescGenericsInstContext())
        {
            return GENERIC_PARAM_CONTEXT_METHODDESC;
        }
        else if (gcInfoDecoder.HasMethodTableGenericsInstContext())
        {
            return GENERIC_PARAM_CONTEXT_METHODTABLE;
        }
        return GENERIC_PARAM_CONTEXT_THIS;
    }
    return GENERIC_PARAM_CONTEXT_NONE;
#endif // USE_GC_INFO_DECODER
}

/*****************************************************************************
 *
 *  Returns the extra argument passed to shared generic code if it is still alive.
 *  Returns NULL in all other cases.
 */
PTR_VOID EECodeManager::GetParamTypeArg(PREGDISPLAY     pContext,
                                        EECodeInfo *    pCodeInfo)

{
    LIMITED_METHOD_DAC_CONTRACT;

#ifndef USE_GC_INFO_DECODER
    unsigned    relOffset = pCodeInfo->GetRelOffset();

    /* Extract the necessary information from the info block header */
    hdrInfo    *hdrInfoBody;
    PTR_CBYTE   table = pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody);

    if (!hdrInfoBody->genericsContext ||
        hdrInfoBody->prologOffs != hdrInfo::NOT_IN_PROLOG ||
        hdrInfoBody->epilogOffs != hdrInfo::NOT_IN_EPILOG)
    {
        return NULL;
    }

    TADDR fp = GetRegdisplayFP(pContext);
    TADDR taParamTypeArg = *PTR_TADDR(fp - GetParamTypeArgOffset(hdrInfoBody));
    return PTR_VOID(taParamTypeArg);

#else // !USE_GC_INFO_DECODER
    return EECodeManager::GetExactGenericsToken(pContext, pCodeInfo);

#endif // USE_GC_INFO_DECODER
}

#if defined(FEATURE_EH_FUNCLETS) && defined(USE_GC_INFO_DECODER)
/*
    Returns the generics token.  This is used by GetInstance() and GetParamTypeArg() on WIN64.
*/
//static
PTR_VOID EECodeManager::GetExactGenericsToken(PREGDISPLAY     pContext,
                                              EECodeInfo *    pCodeInfo)
{
    LIMITED_METHOD_DAC_CONTRACT;

    return EECodeManager::GetExactGenericsToken(GetSP(pContext->pCurrentContext),
                                                GetFP(pContext->pCurrentContext),
                                                pCodeInfo);
}

//static
PTR_VOID EECodeManager::GetExactGenericsToken(TADDR           sp,
                                              TADDR           fp,
                                              EECodeInfo *    pCodeInfo)
{
    LIMITED_METHOD_DAC_CONTRACT;

    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

    GcInfoDecoder gcInfoDecoder(
            gcInfoToken,
            GcInfoDecoderFlags (DECODE_GENERICS_INST_CONTEXT)
            );

    INT32 spOffsetGenericsContext = gcInfoDecoder.GetGenericsInstContextStackSlot();
    if (spOffsetGenericsContext != NO_GENERICS_INST_CONTEXT)
    {
        TADDR baseStackSlot = gcInfoDecoder.HasStackBaseRegister() ? fp : sp;
        TADDR taSlot = (TADDR)( spOffsetGenericsContext + baseStackSlot );
        TADDR taExactGenericsToken = *PTR_TADDR(taSlot);
        return PTR_VOID(taExactGenericsToken);
    }
    return NULL;
}


#endif // FEATURE_EH_FUNCLETS && USE_GC_INFO_DECODER

/*****************************************************************************/

void * EECodeManager::GetGSCookieAddr(PREGDISPLAY     pContext,
                                      EECodeInfo *    pCodeInfo,
                                      unsigned        flags)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    unsigned       relOffset = pCodeInfo->GetRelOffset();

#ifdef FEATURE_EH_FUNCLETS
    if (pCodeInfo->IsFunclet())
    {
        return NULL;
    }
#endif

#ifdef HAS_LIGHTUNWIND
    // LightUnwind does not track sufficient context to compute GS cookie address
    if (flags & LightUnwind)
    {
        return NULL;
    }
#endif

#ifndef USE_GC_INFO_DECODER
    /* Extract the necessary information from the info block header */
    hdrInfo *hdrInfoBody;
    PTR_CBYTE table = pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody);

    if (hdrInfoBody->prologOffs != hdrInfo::NOT_IN_PROLOG ||
        hdrInfoBody->epilogOffs != hdrInfo::NOT_IN_EPILOG ||
        hdrInfoBody->gsCookieOffset == INVALID_GS_COOKIE_OFFSET)
    {
        return NULL;
    }

    if  (hdrInfoBody->ebpFrame)
    {
        DWORD curEBP = GetRegdisplayFP(pContext);

        return PVOID(SIZE_T(curEBP - hdrInfoBody->gsCookieOffset));
    }
    else
    {
        unsigned argSize = GetPushedArgSize(hdrInfoBody, table, relOffset);

        return PVOID(SIZE_T(pContext->SP + argSize + hdrInfoBody->gsCookieOffset));
    }

#else // !USE_GC_INFO_DECODER
    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();
    GcInfoDecoder gcInfoDecoder(
            gcInfoToken,
            DECODE_GS_COOKIE
            );

    INT32 spOffsetGSCookie = gcInfoDecoder.GetGSCookieStackSlot();
    if (spOffsetGSCookie != NO_GS_COOKIE)
    {
        if(relOffset >= gcInfoDecoder.GetGSCookieValidRangeStart())
        {
            TADDR ptr = GetCallerSp(pContext) + spOffsetGSCookie;

            // Detect the end of GS cookie scope by comparing its address with SP
            // gcInfoDecoder.GetGSCookieValidRangeEnd() is not accurate. It does not
            // account for GS cookie going out of scope inside epilog or multiple epilogs.
            return (ptr >= pContext->SP) ? (LPVOID)ptr : nullptr;
        }
    }
    return NULL;

#endif // USE_GC_INFO_DECODER
}

#ifndef USE_GC_INFO_DECODER
/*****************************************************************************
 *
 *  Returns true if the given IP is in the given method's prolog or epilog.
 */
bool EECodeManager::IsInPrologOrEpilog(DWORD       relPCoffset,
                                       GCInfoToken gcInfoToken,
                                       size_t*     prologSize)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    hdrInfo info;

    DecodeGCHdrInfo(gcInfoToken, relPCoffset, &info);

    if (prologSize)
        *prologSize = info.prologSize;

    return ((info.prologOffs != hdrInfo::NOT_IN_PROLOG) ||
            (info.epilogOffs != hdrInfo::NOT_IN_EPILOG));
}

#ifndef FEATURE_EH_FUNCLETS
/*****************************************************************************
 *
 *  Returns true if the given IP is in the synchronized region of the method (valid for synchronized functions only)
*/
bool  EECodeManager::IsInSynchronizedRegion(DWORD       relOffset,
                                            GCInfoToken gcInfoToken,
                                            unsigned    flags)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    hdrInfo info;

    DecodeGCHdrInfo(gcInfoToken, relOffset, &info);

    // We should be called only for synchronized methods
    _ASSERTE(info.syncStartOffset != INVALID_SYNC_OFFSET && info.syncEndOffset != INVALID_SYNC_OFFSET);

    _ASSERTE(info.syncStartOffset < info.syncEndOffset);
    _ASSERTE(info.epilogCnt <= 1);
    _ASSERTE(info.epilogCnt == 0 || info.syncEndOffset <= info.syncEpilogStart);

    return (info.syncStartOffset < relOffset && relOffset < info.syncEndOffset) ||
        (info.syncStartOffset == relOffset && (flags & (ActiveStackFrame|ExecutionAborted))) ||
        // Synchronized methods have at most one epilog. The epilog does not have to be at the end of the method though.
        // Everything after the epilog is also in synchronized region.
        (info.epilogCnt != 0 && info.syncEpilogStart + info.epilogSize <= relOffset);
}
#endif // FEATURE_EH_FUNCLETS
#endif // !USE_GC_INFO_DECODER

/*****************************************************************************
 *
 *  Returns the size of a given function.
 */
size_t EECodeManager::GetFunctionSize(GCInfoToken gcInfoToken)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    } CONTRACTL_END;

#ifndef USE_GC_INFO_DECODER
    // This is called often on hot paths so use an optimized version of DecodeGCHdrInfo
    // that returns just the method size (first word in the table).
    return DecodeGCHdrInfoMethodSize(gcInfoToken);
#else // !USE_GC_INFO_DECODER

    GcInfoDecoder gcInfoDecoder(
            gcInfoToken,
            DECODE_CODE_LENGTH
            );

    UINT32 codeLength = gcInfoDecoder.GetCodeLength();
    _ASSERTE( codeLength > 0 );
    return codeLength;

#endif // USE_GC_INFO_DECODER
}

/*****************************************************************************
*
*  Get information necessary for return address hijacking of the method represented by the gcInfoToken.
*  If it can be hijacked, it sets the returnKind output parameter to the kind of the return value and
*  returns true.
*  If hijacking is not possible for some reason, it return false.
*/
bool EECodeManager::GetReturnAddressHijackInfo(GCInfoToken gcInfoToken X86_ARG(ReturnKind * returnKind))
{
    CONTRACTL{
        NOTHROW;
    GC_NOTRIGGER;
    SUPPORTS_DAC;
    } CONTRACTL_END;

#ifndef USE_GC_INFO_DECODER
    hdrInfo info;

    DecodeGCHdrInfo(gcInfoToken, 0, &info);

    if (info.revPInvokeOffset != INVALID_REV_PINVOKE_OFFSET)
    {
        // Hijacking of UnmanagedCallersOnly method is not allowed
        return false;
    }

    *returnKind = info.returnKind;
    return true;
#else // !USE_GC_INFO_DECODER

    GcInfoDecoder gcInfoDecoder(gcInfoToken, GcInfoDecoderFlags(DECODE_REVERSE_PINVOKE_VAR));

    if (gcInfoDecoder.GetReversePInvokeFrameStackSlot() != NO_REVERSE_PINVOKE_FRAME)
    {
        // Hijacking of UnmanagedCallersOnly method is not allowed
        return false;
    }

    return true;
#endif // USE_GC_INFO_DECODER
}

#ifndef USE_GC_INFO_DECODER
/*****************************************************************************
 *
 *  Returns the size of the frame of the given function.
 */
unsigned int EECodeManager::GetFrameSize(GCInfoToken gcInfoToken)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    hdrInfo info;

    DecodeGCHdrInfo(gcInfoToken, 0, &info);

    // currently only used by E&C callers need to know about doubleAlign
    // in all likelihood
    _ASSERTE(!info.doubleAlign);
    return info.stackSize;
}
#endif // USE_GC_INFO_DECODER

#ifndef DACCESS_COMPILE

/*****************************************************************************/

#ifndef FEATURE_EH_FUNCLETS
const BYTE* EECodeManager::GetFinallyReturnAddr(PREGDISPLAY pReg)
{
    LIMITED_METHOD_CONTRACT;

    return *(const BYTE**)(size_t)(GetRegdisplaySP(pReg));
}

BOOL EECodeManager::LeaveFinally(GCInfoToken gcInfoToken,
                                unsigned offset,
                                PCONTEXT pCtx)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;


    hdrInfo info;

    DecodeGCHdrInfo(gcInfoToken,
                    offset,
                    &info);

    DWORD       nestingLevel;
    GetHandlerFrameInfo(&info, pCtx->Ebp, pCtx->Esp, (DWORD) IGNORE_VAL, NULL, &nestingLevel);

    // Compute an index into the stack-based table of esp values from
    // each level of catch block.
    PTR_TADDR pBaseSPslots = GetFirstBaseSPslotPtr(pCtx->Ebp, &info);
    PTR_TADDR pPrevSlot    = pBaseSPslots - (nestingLevel - 1);

    /* Currently, LeaveFinally() is not used if the finally is invoked in the
       second pass for unwinding. So we expect the finally to be called locally */
    _ASSERTE(*pPrevSlot == LCL_FINALLY_MARK);

    *pPrevSlot = 0; // Zero out the previous shadow ESP

    pCtx->Esp += sizeof(TADDR); // Pop the return value off the stack
    return TRUE;
}

void EECodeManager::LeaveCatch(GCInfoToken gcInfoToken,
                                unsigned offset,
                                PCONTEXT pCtx)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

#ifdef _DEBUG
    TADDR       baseSP;
    DWORD       nestingLevel;
    bool        hasInnerFilter;
    hdrInfo     info;

    DecodeGCHdrInfo(gcInfoToken, offset, &info);
    GetHandlerFrameInfo(&info, pCtx->Ebp, pCtx->Esp, (DWORD) IGNORE_VAL,
                        &baseSP, &nestingLevel, &hasInnerFilter);
//    _ASSERTE(frameType == FR_HANDLER);
//    _ASSERTE(pCtx->Esp == baseSP);
#endif

    return;
}
#else // !FEATURE_EH_FUNCLETS

#ifndef TARGET_WASM

// This is an assembly helper that enables us to call into EH funclets.
EXTERN_C DWORD_PTR STDCALL CallEHFunclet(Object *pThrowable, UINT_PTR pFuncletToInvoke, CONTEXT *pContext, UINT_PTR *pFuncletCallerSP);

// This is an assembly helper that enables us to call into EH filter funclets.
EXTERN_C DWORD_PTR STDCALL CallEHFilterFunclet(Object *pThrowable, TADDR FP, UINT_PTR pFuncletToInvoke, UINT_PTR *pFuncletCallerSP);

typedef DWORD_PTR (HandlerFn)(UINT_PTR uStackFrame, Object* pExceptionObj);

static inline UINT_PTR CastHandlerFn(HandlerFn *pfnHandler)
{
#ifdef TARGET_ARM
    return DataPointerToThumbCode<UINT_PTR, HandlerFn *>(pfnHandler);
#else
    return (UINT_PTR)pfnHandler;
#endif
}

#endif // TARGET_WASM

// Call catch, finally or filter funclet.
// Return value:
// * Catch funclet: address to resume at after the catch returns
// * Finally funclet: unused
// * Filter funclet: result of the filter funclet (EXCEPTION_CONTINUE_SEARCH (0) or EXCEPTION_EXECUTE_HANDLER (1))
DWORD_PTR EECodeManager::CallFunclet(OBJECTREF throwable, void* pHandler, REGDISPLAY *pRD, ExInfo *pExInfo, bool isFilterFunclet)
{
    DWORD_PTR dwResult = 0;
#ifdef TARGET_WASM
    _ASSERTE(!"CallFunclet for WASM not implemented yet");
#else
    HandlerFn* pfnHandler = (HandlerFn*)pHandler;

    // Since the actual caller of the funclet is the assembly helper, pass the reference
    // to the CallerStackFrame instance so that it can be updated.
    UINT_PTR *pFuncletCallerSP = &(pExInfo->m_csfEHClause.SP);

    if (isFilterFunclet)
    {
        // For invoking IL filter funclet, we pass the CallerSP to the funclet using which
        // it will retrieve the framepointer for accessing the locals in the parent
        // method.
        dwResult = CallEHFilterFunclet(OBJECTREFToObject(throwable),
                                       GetFP(pRD->pCurrentContext),
                                       CastHandlerFn(pfnHandler),
                                       pFuncletCallerSP);
    }
    else
    {
        dwResult = CallEHFunclet(OBJECTREFToObject(throwable),
                                 CastHandlerFn(pfnHandler),
                                 pRD->pCurrentContext,
                                 pFuncletCallerSP);
    }

#endif // TARGET_WASM
    return dwResult;
}

void EECodeManager::ResumeAfterCatch(CONTEXT *pContext, size_t targetSSP, bool fIntercepted)
{
    Thread *pThread = GetThread();
    DWORD_PTR dwResumePC = GetIP(pContext);
    UINT_PTR uAbortAddr = 0;
    if (!fIntercepted)
    {
        CopyOSContext(pThread->m_OSContext, pContext);
        uAbortAddr = (UINT_PTR)COMPlusCheckForAbort(dwResumePC);
    }

    if (uAbortAddr)
    {
        STRESS_LOG2(LF_EH, LL_INFO10, "Thread abort in progress, resuming under control: IP=%p, SP=%p\n", dwResumePC, GetSP(pContext));

        // The dwResumePC is passed to the THROW_CONTROL_FOR_THREAD_FUNCTION ASM helper so that
        // it can establish it as its return address and native stack unwinding can work properly.
#ifdef TARGET_AMD64
#ifdef TARGET_UNIX
        pContext->Rdi = dwResumePC;
#else
        pContext->Rcx = dwResumePC;
#endif
#elif defined(TARGET_ARM) || defined(TARGET_ARM64)
        // On ARM & ARM64, we save off the original PC in Lr. This is the same as done
        // in HandleManagedFault for H/W generated exceptions.
        pContext->Lr = dwResumePC;
#elif defined(TARGET_LOONGARCH64) || defined(TARGET_RISCV64)
        pContext->Ra = dwResumePC;
#endif

        SetIP(pContext, uAbortAddr);
    }
    else
    {
        STRESS_LOG2(LF_EH, LL_INFO100, "Resuming after exception at IP=%p, SP=%p\n", GetIP(pContext), GetSP(pContext));
    }

    ClrRestoreNonvolatileContext(pContext, targetSSP);
}

#if defined(HOST_AMD64) && defined(HOST_WINDOWS)

size_t GetSSPForFrameOnCurrentStack(TADDR ip)
{
    size_t *targetSSP = (size_t *)_rdsspq();
    // The SSP we search is pointing to the return address of the frame represented
    // by the passed in IP. So we search for the instruction pointer from
    // the context and return one slot up from there.
    if (targetSSP != NULL)
    {
        while (*targetSSP++ != ip)
        {
        }
    }

    return (size_t)targetSSP;
}

void EECodeManager::UpdateSSP(PREGDISPLAY pRD)
{
    pRD->SSP = GetSSPForFrameOnCurrentStack(pRD->ControlPC);
}
#endif // HOST_AMD64 && HOST_WINDOWS

#ifdef FEATURE_INTERPRETER
DWORD_PTR InterpreterCodeManager::CallFunclet(OBJECTREF throwable, void* pHandler, REGDISPLAY *pRD, ExInfo *pExInfo, bool isFilter)
{
    Thread *pThread = GetThread();
    InterpThreadContext *threadContext = pThread->GetInterpThreadContext();
    if (threadContext == nullptr || threadContext->pStackStart == nullptr)
    {
        COMPlusThrow(kOutOfMemoryException);
    }
    int8_t *sp = threadContext->pStackPointer;

    // This construct ensures that the InterpreterFrame is always stored at a higher address than the
    // InterpMethodContextFrame. This is important for the stack walking code.
    struct Frames
    {
        InterpMethodContextFrame interpMethodContextFrame = {0};
        InterpreterFrame interpreterFrame;

        Frames(TransitionBlock* pTransitionBlock)
        : interpreterFrame(pTransitionBlock, &interpMethodContextFrame)
        {
        }
    }
    frames(NULL);

    // Use the InterpreterFrame address as a representation of the caller SP of the funclet
    // Note: this needs to match what the VirtualUnwindInterpreterCallFrame sets as the SP
    // when it unwinds out of a block of interpreter frames belonging to that InterpreterFrame.
    pExInfo->m_csfEHClause.SP = (TADDR)&frames.interpreterFrame;

    InterpMethodContextFrame *pOriginalFrame = (InterpMethodContextFrame*)GetRegdisplaySP(pRD);

    StackVal retVal;

    frames.interpMethodContextFrame.startIp = pOriginalFrame->startIp;
    frames.interpMethodContextFrame.pStack = isFilter ? sp : pOriginalFrame->pStack;
    frames.interpMethodContextFrame.pRetVal = (int8_t*)&retVal;

    ExceptionClauseArgs exceptionClauseArgs;
    exceptionClauseArgs.ip = (const int32_t *)pHandler;
    exceptionClauseArgs.pFrame = pOriginalFrame;
    exceptionClauseArgs.isFilter = isFilter;
    exceptionClauseArgs.throwable = throwable;

    InterpExecMethod(&frames.interpreterFrame, &frames.interpMethodContextFrame, threadContext, &exceptionClauseArgs);

    frames.interpreterFrame.Pop();

    if (isFilter)
    {
        // The filter funclet returns the result of the filter funclet (EXCEPTION_CONTINUE_SEARCH (0) or EXCEPTION_EXECUTE_HANDLER (1))
        return retVal.data.i;
    }
    else
    {
        // The catch funclet returns the address to resume at after the catch returns.
        return (DWORD_PTR)retVal.data.s;
    }
}

void InterpreterCodeManager::ResumeAfterCatch(CONTEXT *pContext, size_t targetSSP, bool fIntercepted)
{
    Thread *pThread = GetThread();
    InterpreterFrame * pInterpreterFrame = (InterpreterFrame*)pThread->GetFrame();
    TADDR resumeSP = GetSP(pContext);
    TADDR resumeIP = GetIP(pContext);

    ClrCaptureContext(pContext);

    // Unwind to the caller of the Ex.RhThrowEx / Ex.RhThrowHwEx
    Thread::VirtualUnwindToFirstManagedCallFrame(pContext);

#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
    targetSSP = GetSSPForFrameOnCurrentStack(GetIP(pContext));
#endif // HOST_AMD64 && HOST_WINDOWS

    CONTEXT firstNativeContext;
    size_t firstNativeSSP;

    // Find the native frames chain that contains the resumeSP
    do
    {
        // Skip all managed frames upto a native frame
        while (ExecutionManager::IsManagedCode(GetIP(pContext)))
        {
            Thread::VirtualUnwindCallFrame(pContext);
#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
            if (targetSSP != 0)
            {
                targetSSP += sizeof(size_t);
            }
#endif
        }

        // Save the first native context after managed frames. This will be the context where we throw the resume after catch exception from.
        firstNativeContext = *pContext;
        firstNativeSSP = targetSSP;
        // Move over all native frames until we move over the resumeSP
        while ((GetSP(pContext) < resumeSP) && !ExecutionManager::IsManagedCode(GetIP(pContext)))
        {
#ifdef TARGET_UNIX
            PAL_VirtualUnwind(pContext, NULL);
#else
            Thread::VirtualUnwindCallFrame(pContext);
#endif
#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
            if (targetSSP != 0)
            {
                targetSSP += sizeof(size_t);
            }
#endif
        }
    }
    while (GetSP(pContext) < resumeSP);

    ExecuteFunctionBelowContext((PCODE)ThrowResumeAfterCatchException, &firstNativeContext, firstNativeSSP, resumeSP, resumeIP);
}

#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
void InterpreterCodeManager::UpdateSSP(PREGDISPLAY pRD)
{
    InterpreterFrame* pFrame = (InterpreterFrame*)GetFirstArgReg(pRD->pCurrentContext);
    pRD->SSP = pFrame->GetInterpExecMethodSSP();
}
#endif // HOST_AMD64 && HOST_WINDOWS
#endif // FEATURE_INTERPRETER

#endif // !FEATURE_EH_FUNCLETS

#endif // #ifndef DACCESS_COMPILE

#ifdef DACCESS_COMPILE

void EECodeManager::EnumMemoryRegions(CLRDataEnumMemoryFlags flags)
{
    DAC_ENUM_VTHIS();
}

#endif // #ifdef DACCESS_COMPILE


#ifdef TARGET_X86
/*
 *  GetAmbientSP
 *
 *  This function computes the zero-depth stack pointer for the given nesting
 *  level within the method given.  Nesting level is the depth within
 *  try-catch-finally blocks, and is zero based.  It is up to the caller to
 *  supply a valid nesting level value.
 *
 */

TADDR EECodeManager::GetAmbientSP(PREGDISPLAY     pContext,
                                  EECodeInfo     *pCodeInfo,
                                  DWORD           dwRelOffset,
                                  DWORD           nestingLevel)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    } CONTRACTL_END;

    /* Extract the necessary information from the info block header */

    PTR_CBYTE table = nullptr;
    hdrInfo hdrInfoBody = { 0 };
    hdrInfo *pCodeInfoBody = &hdrInfoBody;
    if (pCodeInfo->GetRelOffset() == dwRelOffset)
    {
        table = pCodeInfo->DecodeGCHdrInfo(&pCodeInfoBody);
    }
    else
    {
        table = pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody, dwRelOffset);
    }

#if defined(_DEBUG) && !defined(DACCESS_COMPILE)
    if (trFixContext)
    {
        minipal_log_print_info("GetAmbientSP [%s][%s] for %s.%s: ",
               pCodeInfoBody->ebpFrame?"ebp":"   ",
               pCodeInfoBody->interruptible?"int":"   ",
               "UnknownClass","UnknownMethod");
        minipal_log_flush_info();
    }
#endif // _DEBUG && !DACCESS_COMPILE

    if ((pCodeInfoBody->prologOffs != hdrInfo::NOT_IN_PROLOG) ||
        (pCodeInfoBody->epilogOffs != hdrInfo::NOT_IN_EPILOG))
    {
        return NULL;
    }

    /* make sure that we have an ebp stack frame */

    if (pCodeInfoBody->handlers)
    {
        _ASSERTE(pCodeInfoBody->ebpFrame);

        TADDR      baseSP;
        GetHandlerFrameInfo(pCodeInfoBody,
                            GetRegdisplayFP(pContext),
                            (DWORD) IGNORE_VAL,
                            nestingLevel,
                            &baseSP);

        _ASSERTE((GetRegdisplayFP(pContext) >= baseSP) && (baseSP >= GetRegdisplaySP(pContext)));

        return baseSP;
    }

    _ASSERTE(nestingLevel == 0);

    if (pCodeInfoBody->ebpFrame)
    {
        return GetOutermostBaseFP(GetRegdisplayFP(pContext), pCodeInfoBody);
    }

    TADDR baseSP = GetRegdisplaySP(pContext);
    if  (pCodeInfoBody->interruptible)
    {
        baseSP += scanArgRegTableI(skipToArgReg(*pCodeInfoBody, table),
                                   dwRelOffset,
                                   dwRelOffset,
                                   pCodeInfoBody);
    }
    else
    {
        baseSP += scanArgRegTable(skipToArgReg(*pCodeInfoBody, table),
                                  dwRelOffset,
                                  pCodeInfoBody);
    }

    return baseSP;
}
#endif // TARGET_X86

/*
    Get the number of bytes used for stack parameters.
    This is currently only used on x86.
 */

// virtual
ULONG32 EECodeManager::GetStackParameterSize(EECodeInfo * pCodeInfo)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    } CONTRACTL_END;

#if defined(TARGET_X86)
#if defined(FEATURE_EH_FUNCLETS)
    if (pCodeInfo->IsFunclet())
    {
        // Funclet has no stack argument
        return 0;
    }
#endif // FEATURE_EH_FUNCLETS

    hdrInfo * hdrInfoBody;
    pCodeInfo->DecodeGCHdrInfo(&hdrInfoBody);

    return (ULONG32)::GetStackParameterSize(hdrInfoBody);

#else
    return 0;

#endif // TARGET_X86
}

#ifdef FEATURE_INTERPRETER

static void VirtualUnwindInterpreterCallFrame(TADDR sp, T_CONTEXT *pContext)
{
    PTR_InterpMethodContextFrame pFrame = dac_cast<PTR_InterpMethodContextFrame>(sp);
    pFrame = pFrame->pParent;
    if (pFrame != NULL)
    {
        SetIP(pContext, (TADDR)pFrame->ip);
        SetSP(pContext, dac_cast<TADDR>(pFrame));
        SetFP(pContext, (TADDR)pFrame->pStack);
    }
    else
    {
        // This indicates that there are no more interpreter frames to unwind in the current InterpExecMethod
        // The stack walker will not find any code manager for the address InterpreterFrame::DummyCallerIP (0) 
        // and move on to the next explicit frame which is the InterpreterFrame.
        // The SP is set to the address of the InterpreterFrame. For the case of interpreted exception handling
        // funclets, this matches the pExInfo->m_csfEHClause.SP that the CallFunclet sets.
        // Interpreter-TODO: Consider returning the context of the JITted / AOTed code that called the interpreter instead
        SetIP(pContext, InterpreterFrame::DummyCallerIP);
        TADDR interpreterFrameAddress = GetFirstArgReg(pContext);
        SetSP(pContext, interpreterFrameAddress);
    }
    pContext->ContextFlags = CONTEXT_FULL;
}

bool InterpreterCodeManager::UnwindStackFrame(PREGDISPLAY     pRD,
                                              EECodeInfo     *pCodeInfo,
                                              unsigned        flags)
{
    if (pRD->IsCallerContextValid)
    {
        // We already have the caller's frame context
        // We just switch the pointers
        PT_CONTEXT temp      = pRD->pCurrentContext;
        pRD->pCurrentContext = pRD->pCallerContext;
        pRD->pCallerContext  = temp;

        PT_KNONVOLATILE_CONTEXT_POINTERS tempPtrs = pRD->pCurrentContextPointers;
        pRD->pCurrentContextPointers            = pRD->pCallerContextPointers;
        pRD->pCallerContextPointers             = tempPtrs;
    }
    else
    {
        TADDR sp = (TADDR)GetRegdisplaySP(pRD);
        VirtualUnwindInterpreterCallFrame(sp, pRD->pCurrentContext);
    }

    SyncRegDisplayToCurrentContext(pRD);
    pRD->IsCallerContextValid = FALSE;
    pRD->IsCallerSPValid = FALSE;

    return true;
}

void InterpreterCodeManager::UnwindStackFrame(T_CONTEXT *pContext)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    }
    CONTRACTL_END;

    _ASSERTE(pContext != NULL);
    VirtualUnwindInterpreterCallFrame(GetSP(pContext), pContext);
}

void InterpreterCodeManager::EnsureCallerContextIsValid(PREGDISPLAY  pRD, EECodeInfo * pCodeInfo /*= NULL*/, unsigned flags /*= 0*/)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;
    }
    CONTRACTL_END;

    if( !pRD->IsCallerContextValid )
    {
        // We need to make a copy here (instead of switching the pointers), in order to preserve the current context
        *(pRD->pCallerContext) = *(pRD->pCurrentContext);
        TADDR sp = (TADDR)GetRegdisplaySP(pRD);
        VirtualUnwindInterpreterCallFrame(sp, pRD->pCallerContext);
        // Preserve the context pointers, they are used by floating point registers unwind starting at the original context.
        memcpy(pRD->pCallerContextPointers, pRD->pCurrentContextPointers, sizeof(KNONVOLATILE_CONTEXT_POINTERS));

        pRD->IsCallerContextValid = TRUE;
    }

    _ASSERTE( pRD->IsCallerContextValid );
}

bool InterpreterCodeManager::IsGcSafe(EECodeInfo *pCodeInfo,
                                      DWORD       dwRelOffset)
{
    // Interpreter-TODO: Implement this
    return true;
}

bool InterpreterCodeManager::EnumGcRefs(PREGDISPLAY     pContext,
                                        EECodeInfo     *pCodeInfo,
                                        unsigned        flags,
                                        GCEnumCallback  pCallback,
                                        LPVOID          hCallBack,
                                        DWORD           relOffsetOverride)
{
    CONTRACTL {
        NOTHROW;
        GC_NOTRIGGER;
    } CONTRACTL_END;

    unsigned curOffs = pCodeInfo->GetRelOffset();

#ifdef _DEBUG
    // Get the name of the current method
    const char * methodName = pCodeInfo->GetMethodDesc()->GetName();
    LOG((LF_GCINFO, LL_INFO1000, "Reporting GC refs for %s at offset %04x.\n",
        methodName, curOffs));
#endif

    GCInfoToken gcInfoToken = pCodeInfo->GetGCInfoToken();

#ifdef _DEBUG
    if (flags & ActiveStackFrame)
    {
        InterpreterGcInfoDecoder _gcInfoDecoder(
                                                gcInfoToken,
                                                DECODE_INTERRUPTIBILITY,
                                                curOffs
                                                );
        _ASSERTE(_gcInfoDecoder.IsInterruptible() || _gcInfoDecoder.CouldBeSafePoint());
    }
#endif

    // Check if we have been given an override value for relOffset
    if (relOffsetOverride != NO_OVERRIDE_OFFSET)
    {
        // We've been given an override offset for GC Info
        curOffs = relOffsetOverride;

        LOG((LF_GCINFO, LL_INFO1000, "Adjusted GC reporting offset to provided override offset. Now reporting GC refs for %s at offset %04x.\n",
            methodName, curOffs));
    }


#if defined(FEATURE_EH_FUNCLETS)   // funclets
    if (pCodeInfo->GetJitManager()->IsFilterFunclet(pCodeInfo))
    {
        // Filters are the only funclet that run during the 1st pass, and must have
        // both the leaf and the parent frame reported.  In order to avoid double
        // reporting of the untracked variables, do not report them for the filter.
        flags |= NoReportUntracked;
    }
#endif // FEATURE_EH_FUNCLETS

    bool reportScratchSlots;

    // We report scratch slots only for leaf frames.
    // A frame is non-leaf if we are executing a call, or a fault occurred in the function.
    // The only case in which we need to report scratch slots for a non-leaf frame
    //   is when execution has to be resumed at the point of interruption (via ResumableFrame)
    _ASSERTE( sizeof( BOOL ) >= sizeof( ActiveStackFrame ) );
    reportScratchSlots = (flags & ActiveStackFrame) != 0;


    InterpreterGcInfoDecoder gcInfoDecoder(
                                           gcInfoToken,
                                           GcInfoDecoderFlags (DECODE_GC_LIFETIMES | DECODE_SECURITY_OBJECT | DECODE_VARARG),
                                           curOffs
                                           );

    if (!gcInfoDecoder.EnumerateLiveSlots(
                                          pContext,
                                          reportScratchSlots,
                                          flags,
                                          pCallback,
                                          hCallBack
                                          ))
    {
        return false;
    }

#ifdef FEATURE_EH_FUNCLETS   // funclets
    //
    // If we're in a funclet, we do not want to report the incoming varargs.  This is
    // taken care of by the parent method and the funclet should access those arguments
    // by way of the parent method's stack frame.
    //
    if(pCodeInfo->IsFunclet())
    {
        return true;
    }
#endif // FEATURE_EH_FUNCLETS

    if (gcInfoDecoder.GetIsVarArg())
    {
        // Interpreter-TODO: Implement this
        _ASSERTE(false);
        return false;
    }

    return true;
}

OBJECTREF InterpreterCodeManager::GetInstance(PREGDISPLAY     pContext,
                                              EECodeInfo *    pCodeInfo)
{
    // Interpreter-TODO: Implement this
    return NULL;
}

PTR_VOID InterpreterCodeManager::GetParamTypeArg(PREGDISPLAY     pContext,
                                                 EECodeInfo *    pCodeInfo)
{
    // Interpreter-TODO: Implement this
    return NULL;
}

GenericParamContextType InterpreterCodeManager::GetParamContextType(PREGDISPLAY     pContext,
                                            EECodeInfo *    pCodeInfo)
{
    // Interpreter-TODO: Implement this
    return GENERIC_PARAM_CONTEXT_NONE;
}

size_t InterpreterCodeManager::GetFunctionSize(GCInfoToken gcInfoToken)
{
    // Interpreter-TODO: Implement this
    return 0;
}

#endif // FEATURE_INTERPRETER
