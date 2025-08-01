// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                          BasicBlock                                       XX
XX                                                                           XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/
#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#include "jitstd/algorithm.h"

#if MEASURE_BLOCK_SIZE
/* static  */
size_t BasicBlock::s_Size;
/* static */
size_t BasicBlock::s_Count;
#endif // MEASURE_BLOCK_SIZE

#ifdef DEBUG
// The max # of tree nodes in any BB
/* static */
unsigned BasicBlock::s_nMaxTrees;
#endif // DEBUG

#ifdef DEBUG
FlowEdge* ShuffleHelper(unsigned hash, FlowEdge* res)
{
    FlowEdge* head = res;
    for (FlowEdge* prev = nullptr; res != nullptr; prev = res, res = res->getNextPredEdge())
    {
        unsigned blkHash = (hash ^ (res->getSourceBlock()->bbNum << 16) ^ res->getSourceBlock()->bbNum);
        if (((blkHash % 1879) & 1) && prev != nullptr)
        {
            // Swap res with head.
            prev->setNextPredEdge(head);
            FlowEdge* const resNext  = res->getNextPredEdge();
            FlowEdge* const headNext = head->getNextPredEdge();
            head->setNextPredEdge(resNext);
            res->setNextPredEdge(headNext);
            std::swap(head, res);
        }
    }
    return head;
}

unsigned SsaStressHashHelper()
{
    // hash = 0: turned off, hash = 1: use method hash, hash = *: use custom hash.
    unsigned hash = JitConfig.JitSsaStress();

    if (hash == 0)
    {
        return hash;
    }
    if (hash == 1)
    {
        return JitTls::GetCompiler()->info.compMethodHash();
    }
    return ((hash >> 16) == 0) ? ((hash << 16) | hash) : hash;
}
#endif

//------------------------------------------------------------------------
// setLikelihood: set the likelihood of a flow edge
//
// Arguments:
//   likelihood -- value in range [0.0, 1.0] indicating how likely
//     the source block is to transfer control along this edge.
//
void FlowEdge::setLikelihood(weight_t likelihood)
{
    assert(likelihood >= 0.0);
    assert(likelihood <= 1.0);

#ifdef DEBUG
    if (m_likelihoodSet)
    {
        JITDUMP("setting likelihood of " FMT_BB " -> " FMT_BB " from " FMT_WT " to " FMT_WT "\n", m_sourceBlock->bbNum,
                m_destBlock->bbNum, m_likelihood, likelihood);
    }
    else
    {
        JITDUMP("setting likelihood of " FMT_BB " -> " FMT_BB " to " FMT_WT "\n", m_sourceBlock->bbNum,
                m_destBlock->bbNum, likelihood);
    }

    m_likelihoodSet = true;
#endif // DEBUG

    m_likelihood = likelihood;
}

//------------------------------------------------------------------------
// addLikelihood: adjust the likelihood of a flow edge
//
// Arguments:
//   addedLikelihood -- value in range [-likelihood, 1.0 - likelihood]
//     to add to current likelihood.
//
void FlowEdge::addLikelihood(weight_t addedLikelihood)
{
    assert(m_likelihoodSet);

    weight_t newLikelihood = m_likelihood + addedLikelihood;

    // Tolerate slight overflow or underflow
    //
    const weight_t eps = 0.0001;

    if ((newLikelihood < 0) && (newLikelihood > -eps))
    {
        newLikelihood = 0.0;
    }
    else if ((newLikelihood > 1) && (newLikelihood < 1 + eps))
    {
        newLikelihood = 1.0;
    }

    assert(newLikelihood >= 0.0);
    assert(newLikelihood <= 1.0);

    JITDUMP("updating likelihood of " FMT_BB " -> " FMT_BB " from " FMT_WT " to " FMT_WT "\n", m_sourceBlock->bbNum,
            m_destBlock->bbNum, m_likelihood, newLikelihood);

    m_likelihood = newLikelihood;
}

//------------------------------------------------------------------------
//  AllSuccessorEnumerator: Construct an instance of the enumerator.
//
//  Arguments:
//     comp       - Compiler instance
//     block      - The block whose successors are to be iterated
//     useProfile - If true, determines the order of successors visited using profile data
//
AllSuccessorEnumerator::AllSuccessorEnumerator(Compiler* comp, BasicBlock* block, const bool useProfile /* = false */)
    : m_block(block)
{
    m_numSuccs = 0;
    block->VisitAllSuccs(
        comp,
        [this](BasicBlock* succ) {
        if (m_numSuccs < ArrLen(m_successors))
        {
            m_successors[m_numSuccs] = succ;
        }

        m_numSuccs++;
        return BasicBlockVisit::Continue;
    },
        useProfile);

    if (m_numSuccs > ArrLen(m_successors))
    {
        m_pSuccessors = new (comp, CMK_BasicBlock) BasicBlock*[m_numSuccs];

        unsigned numSuccs = 0;
        block->VisitAllSuccs(
            comp,
            [this, &numSuccs](BasicBlock* succ) {
            assert(numSuccs < m_numSuccs);
            m_pSuccessors[numSuccs++] = succ;
            return BasicBlockVisit::Continue;
        },
            useProfile);

        assert(numSuccs == m_numSuccs);
    }
}

//------------------------------------------------------------------------
// BlockPredsWithEH:
//   Return list of predecessors, including due to EH flow. This is logically
//   the opposite of BasicBlock::VisitAllSuccs.
//
// Arguments:
//    blk - Block to get predecessors for.
//
// Returns:
//    List of edges.
//
FlowEdge* Compiler::BlockPredsWithEH(BasicBlock* blk)
{
    if (!bbIsHandlerBeg(blk))
    {
        return blk->bbPreds;
    }

    BlockToFlowEdgeMap* ehPreds = GetBlockToEHPreds();
    FlowEdge*           res;
    if (ehPreds->Lookup(blk, &res))
    {
        return res;
    }

    res               = blk->bbPreds;
    unsigned tryIndex = blk->getHndIndex();
    // Add all blocks handled by this handler (except for second blocks of BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pairs;
    // these cannot cause transfer to the handler...)
    // TODO-Throughput: It would be nice if we could iterate just over the blocks in the try, via
    // something like:
    //   for (BasicBlock* bb = ehblk->ebdTryBeg; bb != ehblk->ebdTryLast->Next(); bb = bb->Next())
    //     (plus adding in any filter blocks outside the try whose exceptions are handled here).
    // That doesn't work, however: funclets have caused us to sometimes split the body of a try into
    // more than one sequence of contiguous blocks.  We need to find a better way to do this.
    for (BasicBlock* const bb : Blocks())
    {
        if (bbInExnFlowRegions(tryIndex, bb) && !bb->isBBCallFinallyPairTail())
        {
            res = new (this, CMK_FlowEdge) FlowEdge(bb, blk, res);

#if MEASURE_BLOCK_SIZE
            genFlowNodeCnt += 1;
            genFlowNodeSize += sizeof(FlowEdge);
#endif // MEASURE_BLOCK_SIZE
        }
    }

    EHblkDsc* ehblk = ehGetDsc(tryIndex);
    if (ehblk->HasFinallyOrFaultHandler() && (ehblk->ebdHndBeg == blk))
    {
        // block is a finally or fault handler; all enclosing filters are predecessors
        unsigned enclosing = ehblk->ebdEnclosingTryIndex;
        while (enclosing != EHblkDsc::NO_ENCLOSING_INDEX)
        {
            EHblkDsc* enclosingDsc = ehGetDsc(enclosing);
            if (enclosingDsc->HasFilter())
            {
                for (BasicBlock* filterBlk = enclosingDsc->ebdFilter; filterBlk != enclosingDsc->ebdHndBeg;
                     filterBlk             = filterBlk->Next())
                {
                    res = new (this, CMK_FlowEdge) FlowEdge(filterBlk, blk, res);

                    assert(filterBlk->VisitEHEnclosedHandlerSecondPassSuccs(this, [blk](BasicBlock* succ) {
                        return succ == blk ? BasicBlockVisit::Abort : BasicBlockVisit::Continue;
                    }) == BasicBlockVisit::Abort);
                }
            }

            enclosing = enclosingDsc->ebdEnclosingTryIndex;
        }
    }

#ifdef DEBUG
    unsigned hash = SsaStressHashHelper();
    if (hash != 0)
    {
        res = ShuffleHelper(hash, res);
    }
#endif // DEBUG
    ehPreds->Set(blk, res);
    return res;
}

//------------------------------------------------------------------------
// BlockDominancePreds:
//   Return list of dominance predecessors. This is the set that we know for
//   sure contains a block that was fully executed before control reached
//   'blk'.
//
// Arguments:
//   blk - Block to get dominance predecessors for.
//
// Returns:
//   List of edges.
//
// Remarks:
//   Differs from BlockPredsWithEH only in the treatment of handler blocks;
//   enclosed blocks are never dominance preds, while all predecessors of
//   blocks in the 'try' are (currently only the first try block expected).
//
//   There are additional complications due to spurious flow because of
//   two-pass EH. In the flow graph with EH edges we can see entries into the
//   try from filters outside the try, to blocks other than the "try-begin"
//   block. Hence we need to consider the full set of blocks in the try region
//   when considering the block dominance preds.
//
FlowEdge* Compiler::BlockDominancePreds(BasicBlock* blk)
{
    if (!bbIsHandlerBeg(blk))
    {
        return blk->bbPreds;
    }

    BlockToFlowEdgeMap* domPreds = GetDominancePreds();
    FlowEdge*           res;
    if (domPreds->Lookup(blk, &res))
    {
        return res;
    }

    EHblkDsc* ehblk = ehGetBlockHndDsc(blk);
    res             = BlockPredsWithEH(blk);
    for (BasicBlock* predBlk : ehblk->ebdTryBeg->PredBlocks())
    {
        res = new (this, CMK_FlowEdge) FlowEdge(predBlk, blk, res);
    }

    domPreds->Set(blk, res);
    return res;
}

//------------------------------------------------------------------------
// IsInsertedSsaLiveIn: See if a local is marked as being live-in to a block in
// the side table with locals inserted into SSA.
//
// Arguments:
//   block - The block
//   lclNum - The local
//
// Returns:
//    True if the local is marked as live-in to that block
//
bool Compiler::IsInsertedSsaLiveIn(BasicBlock* block, unsigned lclNum)
{
    assert(lvaGetDesc(lclNum)->lvInSsa);

    if (m_insertedSsaLocalsLiveIn == nullptr)
    {
        return false;
    }

    return m_insertedSsaLocalsLiveIn->Lookup(BasicBlockLocalPair(block, lclNum));
}

//------------------------------------------------------------------------
// AddInsertedSsaLiveIn: Mark as local that was inserted into SSA as being
// live-in to a block.
//
// Arguments:
//   block - The block
//   lclNum - The local
//
// Returns:
//    True if this was added anew; false if the local was already marked as such.
//
bool Compiler::AddInsertedSsaLiveIn(BasicBlock* block, unsigned lclNum)
{
    // SSA-inserted locals always have explicit reaching defs for all uses, so
    // it never makes sense for them to be live into the first block.
    assert(block != fgFirstBB);

    if (m_insertedSsaLocalsLiveIn == nullptr)
    {
        m_insertedSsaLocalsLiveIn = new (this, CMK_SSA) BasicBlockLocalPairSet(getAllocator(CMK_SSA));
    }

    if (m_insertedSsaLocalsLiveIn->Set(BasicBlockLocalPair(block, lclNum), true, BasicBlockLocalPairSet::Overwrite))
    {
        return false;
    }

    JITDUMP("Marked V%02u as live into " FMT_BB "\n", lclNum, block->bbNum);
    return true;
}

//------------------------------------------------------------------------
// IsLastHotBlock: see if this is the last block before the cold section
//
// Arguments:
//    compiler - current compiler instance
//
// Returns:
//    true if the next block is fgFirstColdBlock
//    (if fgFirstColdBlock is null, this call is equivalent to IsLast())
//
bool BasicBlock::IsLastHotBlock(Compiler* compiler) const
{
    return (bbNext == compiler->fgFirstColdBlock);
}

//------------------------------------------------------------------------
// IsFirstColdBlock: see if this is the first block in the cold section
//
// Arguments:
//    compiler - current compiler instance
//
// Returns:
//    true if this is fgFirstColdBlock
//    (fgFirstColdBlock is null if there is no cold code)
//
bool BasicBlock::IsFirstColdBlock(Compiler* compiler) const
{
    return (this == compiler->fgFirstColdBlock);
}

//------------------------------------------------------------------------
// CanRemoveJumpToNext: determine if jump to the next block can be omitted
//
// Arguments:
//    compiler - current compiler instance
//
// Returns:
//    true if block is a BBJ_ALWAYS to the next block that we can fall into
//
bool BasicBlock::CanRemoveJumpToNext(Compiler* compiler) const
{
    assert(KindIs(BBJ_ALWAYS));
    return JumpsToNext() && !IsLastHotBlock(compiler);
}

//------------------------------------------------------------------------
// CanRemoveJumpToTarget: determine if jump to target can be omitted
//
// Arguments:
//    target - true/false target of the BBJ_COND block
//    compiler - current compiler instance
//
// Returns:
//    true if block is a BBJ_COND that can fall into target
//
bool BasicBlock::CanRemoveJumpToTarget(BasicBlock* target, Compiler* compiler) const
{
    assert(KindIs(BBJ_COND));
    assert(TrueTargetIs(target) || FalseTargetIs(target));
    return NextIs(target) && !IsLastHotBlock(compiler);
}

#ifdef DEBUG

//------------------------------------------------------------------------
// checkPredListOrder: see if pred list is properly ordered
//
// Returns:
//    false if pred list is not in increasing bbID order.
//
bool BasicBlock::checkPredListOrder()
{
    unsigned lastBBID = 0;
    bool     compare  = false;
    for (BasicBlock* const predBlock : PredBlocks())
    {
        const unsigned bbID = predBlock->bbID;
        if (compare && (bbID <= lastBBID))
        {
            assert(bbID != lastBBID);
            return false;
        }
        compare  = true;
        lastBBID = bbID;
    }
    return true;
}

//------------------------------------------------------------------------
// dspBlockILRange(): Display the block's IL range as [XXX...YYY), where XXX and YYY might be "???" for BAD_IL_OFFSET.
//
void BasicBlock::dspBlockILRange() const
{
    if (bbCodeOffs != BAD_IL_OFFSET)
    {
        printf("[%03X..", bbCodeOffs);
    }
    else
    {
        printf("[???"
               "..");
    }

    if (bbCodeOffsEnd != BAD_IL_OFFSET)
    {
        // brace-matching editor workaround for following line: (
        printf("%03X)", bbCodeOffsEnd);
    }
    else
    {
        // brace-matching editor workaround for following line: (
        printf("???"
               ")");
    }
}

//------------------------------------------------------------------------
// dspFlags: Print out the block's flags
//
void BasicBlock::dspFlags() const
{
    static const struct
    {
        const BasicBlockFlags flag;
        const char* const     displayString;
    } bbFlagDisplay[] = {
        {BBF_IMPORTED, "i"},
        {BBF_IS_LIR, "LIR"},
        {BBF_PROF_WEIGHT, "IBC"},
        {BBF_MARKED, "m"},
        {BBF_REMOVED, "del"},
        {BBF_DONT_REMOVE, "keep"},
        {BBF_INTERNAL, "internal"},
        {BBF_HAS_SUPPRESSGC_CALL, "sup-gc"},
        {BBF_HAS_LABEL, "label"},
        {BBF_HAS_JMP, "jmp"},
        {BBF_HAS_CALL, "hascall"},
        {BBF_DOMINATED_BY_EXCEPTIONAL_ENTRY, "xentry"},
        {BBF_GC_SAFE_POINT, "gcsafe"},
        {BBF_HAS_NEWOBJ, "newobj"},
        {BBF_HAS_NEWARR, "newarr"},
        {BBF_BACKWARD_JUMP, "bwd"},
        {BBF_BACKWARD_JUMP_TARGET, "bwd-target"},
        {BBF_BACKWARD_JUMP_SOURCE, "bwd-src"},
        {BBF_PATCHPOINT, "ppoint"},
        {BBF_PARTIAL_COMPILATION_PATCHPOINT, "pc-ppoint"},
        {BBF_HAS_HISTOGRAM_PROFILE, "hist"},
        {BBF_TAILCALL_SUCCESSOR, "tail-succ"},
        {BBF_RECURSIVE_TAILCALL, "r-tail"},
        {BBF_NO_CSE_IN, "no-cse"},
        {BBF_CAN_ADD_PRED, "add-pred"},
        {BBF_RETLESS_CALL, "retless"},
        {BBF_COLD, "cold"},
        {BBF_KEEP_BBJ_ALWAYS, "KEEP"},
        {BBF_CLONED_FINALLY_BEGIN, "cfb"},
        {BBF_CLONED_FINALLY_END, "cfe"},
        {BBF_LOOP_ALIGN, "align"},
        {BBF_HAS_ALIGN, "has-align"},
        {BBF_HAS_MDARRAYREF, "mdarr"},
        {BBF_NEEDS_GCPOLL, "gcpoll"},
        {BBF_ASYNC_RESUMPTION, "resume"},
    };

    bool first = true;
    for (unsigned i = 0; i < ArrLen(bbFlagDisplay); i++)
    {
        if (HasFlag(bbFlagDisplay[i].flag))
        {
            if (!first)
            {
                printf(" ");
            }
            printf("%s", bbFlagDisplay[i].displayString);
            first = false;
        }
    }
}

/*****************************************************************************
 *
 *  Display the bbPreds basic block list (the block predecessors).
 *  Returns the number of characters printed.
 */

unsigned BasicBlock::dspPreds() const
{
    unsigned count = 0;
    for (FlowEdge* const pred : PredEdges())
    {
        if (count != 0)
        {
            printf(",");
            count += 1;
        }
        printf(FMT_BB, pred->getSourceBlock()->bbNum);
        count += 4;

        // Account for %02u only handling 2 digits, but we can display more than that.
        unsigned digits = CountDigits(pred->getSourceBlock()->bbNum);
        if (digits > 2)
        {
            count += digits - 2;
        }

        // Does this predecessor have an interesting dup count? If so, display it.
        if (pred->getDupCount() > 1)
        {
            printf("(%u)", pred->getDupCount());
            count += 2 + CountDigits(pred->getDupCount());
        }
    }
    return count;
}

//------------------------------------------------------------------------
// dspSuccs: Display the basic block successors.
//
void BasicBlock::dspSuccs() const
{
    bool first = true;
    for (const BasicBlock* const succ : Succs())
    {
        printf("%s" FMT_BB, first ? "" : ",", succ->bbNum);
        first = false;
    }
}

// Display a compact representation of the bbKind, that is, where this block branches.
// This is similar to code in Compiler::fgTableDispBasicBlock(), but doesn't have that code's requirements to align
// things strictly.
void BasicBlock::dspKind() const
{
    auto dspBlockNum = [](const FlowEdge* e) -> const char* {
        static char buffers[3][64]; // static array of 3 to allow 3 concurrent calls in one printf()
        static int  nextBufferIndex = 0;

        auto& buffer              = buffers[nextBufferIndex];
        nextBufferIndex           = (nextBufferIndex + 1) % ArrLen(buffers);
        const size_t sizeOfBuffer = ArrLen(buffer);
        int          written;

        const BasicBlock* b = e->getDestinationBlock();
        if (b == nullptr)
        {
            written = _snprintf_s(buffer, sizeOfBuffer, sizeOfBuffer, "NULL");
        }
        else
        {
            written = _snprintf_s(buffer, sizeOfBuffer, sizeOfBuffer, FMT_BB, b->bbNum);
        }

        const bool printEdgeLikelihoods = true; // TODO: parameterize this?
        if (printEdgeLikelihoods)
        {
            if (e->hasLikelihood())
            {
                written = _snprintf_s(buffer + written, sizeOfBuffer - written, sizeOfBuffer - written, "(" FMT_WT ")",
                                      e->getLikelihood());
            }
        }

        return buffer;
    };

    switch (bbKind)
    {
        case BBJ_EHFINALLYRET:
        {
            printf(" ->");

            // Early in compilation, we display the jump kind before the BBJ_EHFINALLYRET successors have been set.
            if (bbEhfTargets == nullptr)
            {
                printf(" ????");
            }
            else
            {
                for (unsigned i = 0; i < bbEhfTargets->GetSuccCount(); i++)
                {
                    printf("%c%s", (i == 0) ? ' ' : ',', dspBlockNum(bbEhfTargets->GetSucc(i)));
                }
            }

            printf(" (finret)");
            break;
        }

        case BBJ_EHFAULTRET:
            printf(" (falret)");
            break;

        case BBJ_EHFILTERRET:
            printf(" -> %s (fltret)", dspBlockNum(GetTargetEdge()));
            break;

        case BBJ_EHCATCHRET:
            printf(" -> %s (cret)", dspBlockNum(GetTargetEdge()));
            break;

        case BBJ_THROW:
            printf(" (throw)");
            break;

        case BBJ_RETURN:
            printf(" (return)");
            break;

        case BBJ_ALWAYS:
            if (HasFlag(BBF_KEEP_BBJ_ALWAYS))
            {
                printf(" -> %s (ALWAYS)", dspBlockNum(GetTargetEdge()));
            }
            else
            {
                printf(" -> %s (always)", dspBlockNum(GetTargetEdge()));
            }
            break;

        case BBJ_LEAVE:
            printf(" -> %s (leave)", dspBlockNum(GetTargetEdge()));
            break;

        case BBJ_CALLFINALLY:
            printf(" -> %s (callf)", dspBlockNum(GetTargetEdge()));
            break;

        case BBJ_CALLFINALLYRET:
            printf(" -> %s (callfr)", dspBlockNum(GetTargetEdge()));
            break;

        case BBJ_COND:
            printf(" -> %s,%s (cond)", dspBlockNum(GetTrueEdge()), dspBlockNum(GetFalseEdge()));
            break;

        case BBJ_SWITCH:
        {
            printf(" ->");

            const unsigned   jumpCnt = bbSwtTargets->GetCaseCount();
            FlowEdge** const jumpTab = bbSwtTargets->GetCases();

            for (unsigned i = 0; i < jumpCnt; i++)
            {
                printf("%c%s", (i == 0) ? ' ' : ',', dspBlockNum(jumpTab[i]));

                const bool isDefault = bbSwtTargets->HasDefaultCase() && (i == jumpCnt - 1);
                if (isDefault)
                {
                    printf("[def]");
                }

                const bool isDominant = bbSwtTargets->HasDominantCase() && (i == bbSwtTargets->GetDominantCase());
                if (isDominant)
                {
                    printf("[dom]");
                }
            }

            printf(" (switch)");
        }
        break;

        default:
            unreached();
            break;
    }
}

void BasicBlock::dspBlockHeader(bool showKind /*= true*/, bool showFlags /*= false*/, bool showPreds /*= true*/) const
{
    printf("%s ", dspToString());
    dspBlockILRange();
    if (showKind)
    {
        dspKind();
    }
    if (showPreds)
    {
        printf(", preds={");
        dspPreds();
        printf("} succs={");
        dspSuccs();
        printf("}");
    }
    if (showFlags)
    {
        const unsigned lowFlags  = (unsigned)bbFlags;
        const unsigned highFlags = (unsigned)(bbFlags >> 32);
        printf(" flags=0x%08x.%08x: ", highFlags, lowFlags);
        dspFlags();
    }
    printf("\n");
}

const char* BasicBlock::dspToString(int blockNumPadding /* = 0 */) const
{
    static char buffers[3][64]; // static array of 3 to allow 3 concurrent calls in one printf()
    static int  nextBufferIndex = 0;

    auto& buffer    = buffers[nextBufferIndex];
    nextBufferIndex = (nextBufferIndex + 1) % ArrLen(buffers);
    _snprintf_s(buffer, ArrLen(buffer), ArrLen(buffer), FMT_BB "%*s [%04u]", bbNum, blockNumPadding, "", bbID);
    return buffer;
}

#endif // DEBUG

// Allocation function for MemoryPhiArg.
void* BasicBlock::MemoryPhiArg::operator new(size_t sz, Compiler* comp)
{
    return comp->getAllocator(CMK_MemoryPhiArg).allocate<char>(sz);
}

//------------------------------------------------------------------------
// CloneBlockState: Try to populate `to` block with a copy of `from` block's statements, replacing
//                  uses of local `varNum` with IntCns `varVal`.
//
// Arguments:
//    compiler - Jit compiler instance
//    to - New/empty block to copy statements into
//    from - Block to copy statements from
//
// Note:
//    Leaves block ref count at zero, and pred edge list empty.
//
void BasicBlock::CloneBlockState(Compiler* compiler, BasicBlock* to, const BasicBlock* from)
{
    assert(to->bbStmtList == nullptr);
    to->CopyFlags(from);
    to->bbWeight = from->bbWeight;
    to->copyEHRegion(from);
    to->bbCatchTyp    = from->bbCatchTyp;
    to->bbStkTempsIn  = from->bbStkTempsIn;
    to->bbStkTempsOut = from->bbStkTempsOut;
    to->bbStkDepth    = from->bbStkDepth;
    to->bbCodeOffs    = from->bbCodeOffs;
    to->bbCodeOffsEnd = from->bbCodeOffsEnd;
#ifdef DEBUG
    to->bbTgtStkDepth = from->bbTgtStkDepth;
#endif // DEBUG

    for (Statement* const fromStmt : from->Statements())
    {
        GenTree* newExpr = compiler->gtCloneExpr(fromStmt->GetRootNode());
        assert(newExpr != nullptr);
        compiler->fgInsertStmtAtEnd(to, compiler->fgNewStmtFromTree(newExpr, fromStmt->GetDebugInfo()));
    }
}

//------------------------------------------------------------------------
// TransferTarget: Like CopyTarget, but copies the target descriptors for block types which have
// them (BBJ_SWITCH/BBJ_EHFINALLYRET), that is, take their memory, after which the `from` block
// target is invalid.
//
// Arguments:
//    from - Block to transfer from
//
void BasicBlock::TransferTarget(BasicBlock* from)
{
    switch (from->GetKind())
    {
        case BBJ_SWITCH:
            SetSwitch(from->GetSwitchTargets());
            from->bbSwtTargets = nullptr; // Make sure nobody uses the descriptor after this.
            break;
        case BBJ_EHFINALLYRET:
            SetEhf(from->GetEhfTargets());
            from->bbEhfTargets = nullptr; // Make sure nobody uses the descriptor after this.
            break;

        // TransferTarget may be called after setting the source block of `from`'s
        // successor edges to this block.
        // This means calling GetTarget/GetTrueTarget/GetFalseTarget would trigger asserts.
        // Avoid this by accessing the edges directly.
        case BBJ_COND:
            SetCond(from->bbTrueEdge, from->bbFalseEdge);
            break;
        case BBJ_ALWAYS:
        case BBJ_CALLFINALLY:
        case BBJ_CALLFINALLYRET:
        case BBJ_EHCATCHRET:
        case BBJ_EHFILTERRET:
        case BBJ_LEAVE:
            SetKindAndTargetEdge(from->GetKind(), from->bbTargetEdge);
            break;
        default:
            SetKindAndTargetEdge(from->GetKind()); // Clear the target
            break;
    }
    assert(KindIs(from->GetKind()));
}

// LIR helpers
void BasicBlock::MakeLIR(GenTree* firstNode, GenTree* lastNode)
{
    assert(!IsLIR());
    assert((firstNode == nullptr) == (lastNode == nullptr));
    assert((firstNode == lastNode) || firstNode->Precedes(lastNode));

    m_firstNode = firstNode;
    m_lastNode  = lastNode;
    SetFlags(BBF_IS_LIR);
}

bool BasicBlock::IsLIR() const
{
    assert(isValid());
    return HasFlag(BBF_IS_LIR);
}

//------------------------------------------------------------------------
// firstStmt: Returns the first statement in the block
//
// Return Value:
//    The first statement in the block's bbStmtList.
//
Statement* BasicBlock::firstStmt() const
{
    return bbStmtList;
}

//------------------------------------------------------------------------
// hasSingleStmt: Returns true if block has a single statement
//
// Return Value:
//    true if block has a single statement, false otherwise
//
bool BasicBlock::hasSingleStmt() const
{
    return (firstStmt() != nullptr) && (firstStmt() == lastStmt());
}

//------------------------------------------------------------------------
// lastStmt: Returns the last statement in the block
//
// Return Value:
//    The last statement in the block's bbStmtList.
//
Statement* BasicBlock::lastStmt() const
{
    if (bbStmtList == nullptr)
    {
        return nullptr;
    }

    Statement* result = bbStmtList->GetPrevStmt();
    assert(result != nullptr && result->GetNextStmt() == nullptr);
    return result;
}

//------------------------------------------------------------------------
// BasicBlock::lastNode: Returns the last node in the block.
//
GenTree* BasicBlock::lastNode() const
{
    return IsLIR() ? m_lastNode : lastStmt()->GetRootNode();
}

//------------------------------------------------------------------------
// GetUniquePred: Returns the unique predecessor of a block, if one exists.
// The predecessor lists must be accurate.
//
// Arguments:
//    None.
//
// Return Value:
//    The unique predecessor of a block, or nullptr if there is no unique predecessor.
//
// Notes:
//    If the first block has a predecessor (which it may have, if it is the target of
//    a backedge), we never want to consider it "unique" because the prolog is an
//    implicit predecessor.

BasicBlock* BasicBlock::GetUniquePred(Compiler* compiler) const
{
    assert(compiler->fgPredsComputed);

    if ((bbPreds == nullptr) || (bbPreds->getNextPredEdge() != nullptr) || (this == compiler->fgFirstBB))
    {
        return nullptr;
    }
    else
    {
        return bbPreds->getSourceBlock();
    }
}

//------------------------------------------------------------------------
// GetUniqueSucc: Returns the unique successor of a block, if one exists.
// Only considers BBJ_ALWAYS block types.
//
// Arguments:
//    None.
//
// Return Value:
//    The unique successor of a block, or nullptr if there is no unique successor.
//
BasicBlock* BasicBlock::GetUniqueSucc() const
{
    return KindIs(BBJ_ALWAYS) ? GetTarget() : nullptr;
}

// Static vars.
BasicBlock::MemoryPhiArg* BasicBlock::EmptyMemoryPhiDef = (BasicBlock::MemoryPhiArg*)0x1;

unsigned JitPtrKeyFuncs<BasicBlock>::GetHashCode(const BasicBlock* ptr)
{
#ifdef DEBUG
    unsigned hash = SsaStressHashHelper();
    if (hash != 0)
    {
        return (hash ^ (ptr->bbID << 16) ^ ptr->bbID);
    }
#endif
    return ptr->bbID;
}

//------------------------------------------------------------------------
// isEmpty: check if block is empty or contains only ignorable statements
//
// Return Value:
//    True if block is empty, or contains only PHI stores,
//    or contains zero or more PHI stores followed by NOPs.
//
bool BasicBlock::isEmpty() const
{
    if (!IsLIR())
    {
        for (Statement* const stmt : NonPhiStatements())
        {
            if (!stmt->GetRootNode()->OperIs(GT_NOP))
            {
                return false;
            }
        }
    }
    else
    {
        for (GenTree* node : LIR::AsRange(this))
        {
            if (!node->OperIs(GT_IL_OFFSET))
            {
                return false;
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------
// isValid: Checks that the basic block doesn't mix statements and LIR lists.
//
// Return Value:
//    True if it a valid basic block.
//
bool BasicBlock::isValid() const
{
    const bool isLIR = HasFlag(BBF_IS_LIR);
    if (isLIR)
    {
        // Should not have statements in LIR.
        return (bbStmtList == nullptr);
    }
    else
    {
        // Should not have tree list before LIR.
        return (GetFirstLIRNode() == nullptr);
    }
}

Statement* BasicBlock::FirstNonPhiDef() const
{
    Statement* stmt = firstStmt();
    while ((stmt != nullptr) && stmt->IsPhiDefnStmt())
    {
        stmt = stmt->GetNextStmt();
    }

    return stmt;
}

Statement* BasicBlock::FirstNonPhiDefOrCatchArgStore() const
{
    Statement* stmt = FirstNonPhiDef();
    if (stmt == nullptr)
    {
        return nullptr;
    }
    GenTree* tree = stmt->GetRootNode();
    if (tree->OperIs(GT_STORE_LCL_VAR) && tree->AsLclVar()->Data()->OperIs(GT_CATCH_ARG))
    {
        stmt = stmt->GetNextStmt();
    }
    return stmt;
}

//------------------------------------------------------------------------
// NumSucc: Returns the count of block successors. See the declaration comment for details.
//
// Arguments:
//    None.
//
// Return Value:
//    Count of block successors.
//
unsigned BasicBlock::NumSucc() const
{
    switch (bbKind)
    {
        case BBJ_THROW:
        case BBJ_RETURN:
        case BBJ_EHFAULTRET:
            return 0;

        case BBJ_CALLFINALLY:
        case BBJ_CALLFINALLYRET:
        case BBJ_ALWAYS:
        case BBJ_EHCATCHRET:
        case BBJ_EHFILTERRET:
        case BBJ_LEAVE:
            return 1;

        case BBJ_COND:
            if (bbTrueEdge == bbFalseEdge)
            {
                return 1;
            }
            else
            {
                return 2;
            }

        case BBJ_EHFINALLYRET:
            // We may call this method before we realize we have invalid IL. Tolerate.
            //
            if (!hasHndIndex())
            {
                return 0;
            }

            // We may call this before we've computed the BBJ_EHFINALLYRET successors in the importer. Tolerate.
            //
            if (bbEhfTargets == nullptr)
            {
                return 0;
            }

            return bbEhfTargets->GetSuccCount();

        case BBJ_SWITCH:
            return bbSwtTargets->GetSuccCount();

        default:
            unreached();
    }
}

//------------------------------------------------------------------------
// GetSuccEdge: Returns the requested successor edge. See the declaration comment for details.
//
// Arguments:
//    i - index of successor to return. 0 <= i <= NumSucc().
//
// Return Value:
//    Requested successor edge
//
FlowEdge* BasicBlock::GetSuccEdge(unsigned i) const
{
    assert(i < NumSucc()); // Index bounds check.
    switch (bbKind)
    {
        case BBJ_CALLFINALLY:
        case BBJ_CALLFINALLYRET:
        case BBJ_ALWAYS:
        case BBJ_EHCATCHRET:
        case BBJ_EHFILTERRET:
        case BBJ_LEAVE:
            return GetTargetEdge();

        case BBJ_COND:
            if (i == 0)
            {
                return GetFalseEdge();
            }
            else
            {
                assert(i == 1);
                assert(bbTrueEdge != bbFalseEdge);
                return GetTrueEdge();
            }

        case BBJ_EHFINALLYRET:
            return bbEhfTargets->GetSucc(i);

        case BBJ_SWITCH:
            return bbSwtTargets->GetSucc(i);

        default:
            unreached();
    }
}

//------------------------------------------------------------------------
// GetSucc: Returns the requested block successor. See the declaration comment for details.
//
// Arguments:
//    i - index of successor to return. 0 <= i <= NumSucc().
//
// Return Value:
//    Requested successor block
//
BasicBlock* BasicBlock::GetSucc(unsigned i) const
{
    return GetSuccEdge(i)->getDestinationBlock();
}

void BasicBlock::InitVarSets(Compiler* comp)
{
    VarSetOps::AssignNoCopy(comp, bbVarUse, VarSetOps::MakeEmpty(comp));
    VarSetOps::AssignNoCopy(comp, bbVarDef, VarSetOps::MakeEmpty(comp));
    VarSetOps::AssignNoCopy(comp, bbLiveIn, VarSetOps::MakeEmpty(comp));
    VarSetOps::AssignNoCopy(comp, bbLiveOut, VarSetOps::MakeEmpty(comp));

    bbMemoryUse     = emptyMemoryKindSet;
    bbMemoryDef     = emptyMemoryKindSet;
    bbMemoryLiveIn  = emptyMemoryKindSet;
    bbMemoryLiveOut = emptyMemoryKindSet;
}

// Returns true if the basic block ends with GT_JMP
bool BasicBlock::endsWithJmpMethod(Compiler* comp) const
{
    if (comp->compJmpOpUsed && (bbKind == BBJ_RETURN) && HasFlag(BBF_HAS_JMP))
    {
        GenTree* lastNode = this->lastNode();
        assert(lastNode != nullptr);
        return lastNode->OperIs(GT_JMP);
    }

    return false;
}

// Returns true if the basic block ends with either
//  i) GT_JMP or
// ii) tail call (implicit or explicit)
//
// Params:
//    comp              - Compiler instance
//    fastTailCallsOnly - Only consider fast tail calls excluding tail calls via helper.
//
bool BasicBlock::endsWithTailCallOrJmp(Compiler* comp, bool fastTailCallsOnly /*=false*/) const
{
    GenTreeCall* tailCall                       = nullptr;
    bool         tailCallsConvertibleToLoopOnly = false;
    return endsWithJmpMethod(comp) ||
           endsWithTailCall(comp, fastTailCallsOnly, tailCallsConvertibleToLoopOnly, &tailCall);
}

//------------------------------------------------------------------------------
// endsWithTailCall : Check if the block ends with a tail call.
//
// Arguments:
//    comp                            - compiler instance
//    fastTailCallsOnly               - check for fast tail calls only
//    tailCallsConvertibleToLoopOnly  - check for tail calls convertible to loop only
//    tailCall                        - a pointer to a tree that will be set to the call tree if the block
//                                      ends with a tail call and will be set to nullptr otherwise.
//
// Return Value:
//    true if the block ends with a tail call; false otherwise.
//
// Notes:
//    At most one of fastTailCallsOnly and tailCallsConvertibleToLoopOnly flags can be true.
//
bool BasicBlock::endsWithTailCall(Compiler*     comp,
                                  bool          fastTailCallsOnly,
                                  bool          tailCallsConvertibleToLoopOnly,
                                  GenTreeCall** tailCall) const
{
    assert(!fastTailCallsOnly || !tailCallsConvertibleToLoopOnly);
    *tailCall   = nullptr;
    bool result = false;

    // Is this a tail call?
    // The reason for keeping this under RyuJIT is so as not to impact existing Jit32 x86 and arm
    // targets.
    if (comp->compTailCallUsed)
    {
        if (fastTailCallsOnly || tailCallsConvertibleToLoopOnly)
        {
            // Only fast tail calls or only tail calls convertible to loops
            result = HasFlag(BBF_HAS_JMP) && (bbKind == BBJ_RETURN);
        }
        else
        {
            // Fast tail calls, tail calls convertible to loops, and tails calls dispatched via helper
            result = (bbKind == BBJ_THROW) || (HasFlag(BBF_HAS_JMP) && (bbKind == BBJ_RETURN));
        }

        if (result)
        {
            GenTree* lastNode = this->lastNode();
            if (lastNode->OperIs(GT_CALL))
            {
                GenTreeCall* call = lastNode->AsCall();
                if (tailCallsConvertibleToLoopOnly)
                {
                    result = call->IsTailCallConvertibleToLoop();
                }
                else if (fastTailCallsOnly)
                {
                    result = call->IsFastTailCall();
                }
                else
                {
                    result = call->IsTailCall();
                }

                if (result)
                {
                    *tailCall = call;
                }
            }
            else
            {
                result = false;
            }
        }
    }

    return result;
}

//------------------------------------------------------------------------------
// endsWithTailCallConvertibleToLoop : Check if the block ends with a tail call convertible to loop.
//
// Arguments:
//    comp  -  compiler instance
//    tailCall  -  a pointer to a tree that will be set to the call tree if the block
//                 ends with a tail call convertible to loop and will be set to nullptr otherwise.
//
// Return Value:
//    true if the block ends with a tail call convertible to loop.
//
bool BasicBlock::endsWithTailCallConvertibleToLoop(Compiler* comp, GenTreeCall** tailCall) const
{
    bool fastTailCallsOnly              = false;
    bool tailCallsConvertibleToLoopOnly = true;
    return endsWithTailCall(comp, fastTailCallsOnly, tailCallsConvertibleToLoopOnly, tailCall);
}

/*****************************************************************************
 *
 *  Allocate a basic block but don't append it to the current BB list.
 */

BasicBlock* BasicBlock::New(Compiler* compiler)
{
    BasicBlock* block;

    /* Allocate the block descriptor and zero it out */
    assert(compiler->fgSafeBasicBlockCreation);

    block = new (compiler, CMK_BasicBlock) BasicBlock;

#if MEASURE_BLOCK_SIZE
    BasicBlock::s_Count += 1;
    BasicBlock::s_Size += sizeof(*block);
#endif

#ifdef DEBUG
    // fgLookupBB() is invalid until fgInitBBLookup() is called again.
    compiler->fgInvalidateBBLookup();
#endif

    // TODO-Throughput: The following memset is pretty expensive - do something else?
    // Note that some fields have to be initialized to 0.
    memset((void*)block, 0, sizeof(*block));

    // scopeInfo needs to be able to differentiate between blocks which
    // correspond to some instrs (and so may have some LocalVarInfo
    // boundaries), or have been inserted by the JIT
    block->bbCodeOffs    = BAD_IL_OFFSET;
    block->bbCodeOffsEnd = BAD_IL_OFFSET;
    block->bbID          = compiler->compBasicBlockID++;

    /* Give the block a number, set the ancestor count and weight */

    ++compiler->fgBBcount;
    block->bbNum = ++compiler->fgBBNumMax;

    if (compiler->compRationalIRForm)
    {
        block->SetFlags(BBF_IS_LIR);
    }

    block->bbRefs   = 1;
    block->bbWeight = BB_UNITY_WEIGHT;

    block->bbStkTempsIn  = NO_BASE_TMP;
    block->bbStkTempsOut = NO_BASE_TMP;

    block->bbEntryState = nullptr;

#ifdef DEBUG
    if (compiler->verbose)
    {
        printf("New Basic Block %s created.\n", block->dspToString());
    }
#endif

    // We will give all the blocks var sets after the number of tracked variables
    // is determined and frozen.  After that, if we dynamically create a basic block,
    // we will initialize its var sets.
    if (compiler->fgBBVarSetsInited)
    {
        VarSetOps::AssignNoCopy(compiler, block->bbVarUse, VarSetOps::MakeEmpty(compiler));
        VarSetOps::AssignNoCopy(compiler, block->bbVarDef, VarSetOps::MakeEmpty(compiler));
        VarSetOps::AssignNoCopy(compiler, block->bbLiveIn, VarSetOps::MakeEmpty(compiler));
        VarSetOps::AssignNoCopy(compiler, block->bbLiveOut, VarSetOps::MakeEmpty(compiler));
    }
    else
    {
        VarSetOps::AssignNoCopy(compiler, block->bbVarUse, VarSetOps::UninitVal());
        VarSetOps::AssignNoCopy(compiler, block->bbVarDef, VarSetOps::UninitVal());
        VarSetOps::AssignNoCopy(compiler, block->bbLiveIn, VarSetOps::UninitVal());
        VarSetOps::AssignNoCopy(compiler, block->bbLiveOut, VarSetOps::UninitVal());
    }

    block->bbMemoryUse     = emptyMemoryKindSet;
    block->bbMemoryDef     = emptyMemoryKindSet;
    block->bbMemoryLiveIn  = emptyMemoryKindSet;
    block->bbMemoryLiveOut = emptyMemoryKindSet;

    for (MemoryKind memoryKind : allMemoryKinds())
    {
        block->bbMemorySsaPhiFunc[memoryKind] = nullptr;
        block->bbMemorySsaNumIn[memoryKind]   = 0;
        block->bbMemorySsaNumOut[memoryKind]  = 0;
    }

    block->bbPreorderNum  = 0;
    block->bbPostorderNum = 0;

    return block;
}

BasicBlock* BasicBlock::New(Compiler* compiler, BBKinds kind)
{
    BasicBlock* block = BasicBlock::New(compiler);
    block->bbKind     = kind;

    if (block->KindIs(BBJ_THROW))
    {
        block->bbSetRunRarely();
    }

    return block;
}

BasicBlock* BasicBlock::New(Compiler* compiler, BBJumpTable* ehfTargets)
{
    BasicBlock* block = BasicBlock::New(compiler);
    block->SetEhf(ehfTargets);
    return block;
}

BasicBlock* BasicBlock::New(Compiler* compiler, BBswtDesc* swtTargets)
{
    BasicBlock* block = BasicBlock::New(compiler);
    block->SetSwitch(swtTargets);
    return block;
}

BasicBlock* BasicBlock::New(Compiler* compiler, BBKinds kind, unsigned targetOffs)
{
    BasicBlock* block   = BasicBlock::New(compiler);
    block->bbKind       = kind;
    block->bbTargetOffs = targetOffs;
    return block;
}

//------------------------------------------------------------------------
// isBBCallFinallyPair: Determine if this is the first block of a BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pair
//
// Return Value:
//    True iff "this" is the first block of a BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pair
//    -- a block corresponding to an exit from the try of a try/finally.
//
// Notes:
//    In the flow graph, this becomes a block that calls the finally, and a second, immediately
//    following (in the bbNext chain) empty block to which the finally will return, and which
//    branches unconditionally to the next block to be executed outside the try/finally.
//    Note that code is often generated differently than this description. For example, on x86,
//    the target of the BBJ_CALLFINALLYRET is pushed on the stack and a direct jump is
//    made to the 'finally'. The effect is that the 'finally' returns directly to the target of
//    the BBJ_CALLFINALLYRET. A "retless" BBJ_CALLFINALLY is one that has no corresponding BBJ_CALLFINALLYRET.
//    This can happen if the finally is known to not return (e.g., it contains a 'throw'). In
//    that case, the BBJ_CALLFINALLY flags has BBF_RETLESS_CALL set.
//
bool BasicBlock::isBBCallFinallyPair() const
{
    if (this->KindIs(BBJ_CALLFINALLY) && !this->HasFlag(BBF_RETLESS_CALL))
    {
        // Some asserts that the next block is of the proper form.
        assert(!this->IsLast());
        assert(this->Next()->KindIs(BBJ_CALLFINALLYRET));
        assert(this->Next()->isEmpty());

        return true;
    }
    else
    {
        return false;
    }
}

//------------------------------------------------------------------------
// isBBCallFinallyPairTail: Determine if this is the last block of a BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pair
//
// Return Value:
//    True iff "this" is the last block of a BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pair
//    -- a block corresponding to an exit from the try of a try/finally.
//
// Notes:
//    See notes on isBBCallFinallyPair(), above.
//
bool BasicBlock::isBBCallFinallyPairTail() const
{
    if (KindIs(BBJ_CALLFINALLYRET))
    {
        // Some asserts that the previous block is of the proper form.
        assert(!this->IsFirst());
        assert(this->Prev()->KindIs(BBJ_CALLFINALLY));
        assert(!this->Prev()->HasFlag(BBF_RETLESS_CALL));

        return true;
    }
    else
    {
        return false;
    }
}

//------------------------------------------------------------------------
// hasEHBoundaryIn: Determine if this block begins at an EH boundary.
//
// Return Value:
//    True iff the block is the target of an EH edge; false otherwise.
//
// Notes:
//    For the purposes of this method (and its callers), an EH edge is one on
//    which the EH flow model requires that all lclVars must be reloaded from
//    the stack before use, since control flow may transfer to this block through
//    control flow that is not reflected in the flowgraph.
//    Note that having a predecessor in a different EH region doesn't require
//    that lclVars must be reloaded from the stack. That's only required when
//    this block might be entered via flow that is not represented by an edge
//    in the flowgraph.
//
bool BasicBlock::hasEHBoundaryIn() const
{
    return (bbCatchTyp != BBCT_NONE);
}

//------------------------------------------------------------------------
// hasEHBoundaryOut: Determine if this block ends in an EH boundary.
//
// Return Value:
//    True iff the block ends in an exception boundary that requires that no lclVars
//    are live in registers; false otherwise.
//
// Notes:
//    We may have a successor in a different EH region, but it is OK to have lclVars
//    live in registers if any successor is a normal flow edge. That's because the
//    EH write-thru semantics ensure that we always have an up-to-date value on the stack.
//
bool BasicBlock::hasEHBoundaryOut() const
{
    return KindIs(BBJ_EHFILTERRET, BBJ_EHFINALLYRET, BBJ_EHFAULTRET, BBJ_EHCATCHRET);
}

//------------------------------------------------------------------------
// BBswtDesc copy ctor: copy a switch descriptor
//
// Arguments:
//    comp - compiler instance
//    other - existing switch descriptor to copy
//
BBswtDesc::BBswtDesc(Compiler* comp, const BBswtDesc* other)
    : BBJumpTable(new(comp, CMK_FlowEdge) FlowEdge*[other->succCount + other->caseCount], other -> succCount)
    , caseCount(other->caseCount)
    , cases(succs + succCount)
    , bbsDominantCase(other->bbsDominantCase)
    , bbsHasDefault(other->bbsHasDefault)
    , bbsHasDominantCase(other->bbsHasDominantCase)
{
    // Fill in the new tables
    //
    memcpy(succs, other->succs, sizeof(FlowEdge*) * succCount);
    memcpy(cases, other->cases, sizeof(FlowEdge*) * caseCount);
}

//------------------------------------------------------------------------
// BBJumpTable copy ctor: copy a N-successor block descriptor
//
// Arguments:
//    comp - compiler instance
//    other - existing descriptor to copy
//
BBJumpTable::BBJumpTable(Compiler* comp, const BBJumpTable* other)
    : succs(new(comp, CMK_FlowEdge) FlowEdge*[other->succCount])
    , succCount(other->succCount)
{
    // Fill in the new jump table
    //
    memcpy(succs, other->succs, sizeof(FlowEdge*) * succCount);
}

//------------------------------------------------------------------------
// getCalledCount: get the value used to normalized weights for this method
//
// Arguments:
//    compiler - Compiler instance
//
// Notes:
//   If we don't have profile data then getCalledCount will return BB_UNITY_WEIGHT (100)
//   otherwise it returns the number of times that profile data says the method was called.

// static
weight_t BasicBlock::getCalledCount(Compiler* comp)
{
    // when we don't have profile data then fgCalledCount will be BB_UNITY_WEIGHT (100)
    weight_t calledCount = comp->fgCalledCount;

    // If we haven't yet reach the place where we setup fgCalledCount it could still be zero
    // so return a reasonable value to use until we set it.
    //
    if (calledCount == 0)
    {
        if (comp->fgIsUsingProfileWeights())
        {
            // When we use profile data block counts we have exact counts,
            // not multiples of BB_UNITY_WEIGHT (100)
            calledCount = 1;
        }
        else
        {
            calledCount = comp->fgFirstBB->bbWeight;

            if (calledCount == 0)
            {
                calledCount = BB_UNITY_WEIGHT;
            }
        }
    }
    return calledCount;
}

//------------------------------------------------------------------------
// getBBWeight: get the normalized weight of this block
//
// Arguments:
//    compiler - Compiler instance
//
// Notes:
//    With profile data: number of expected executions of this block, given
//    one call to the method.
//
weight_t BasicBlock::getBBWeight(Compiler* comp) const
{
    if (this->bbWeight == BB_ZERO_WEIGHT)
    {
        return BB_ZERO_WEIGHT;
    }
    else
    {
        weight_t calledCount = getCalledCount(comp);

        // Normalize the bbWeight.
        //
        weight_t fullResult = (this->bbWeight / calledCount) * BB_UNITY_WEIGHT;

        return fullResult;
    }
}

//------------------------------------------------------------------------
// bbStackDepthOnEntry: return depth of IL stack at block entry
//
unsigned BasicBlock::bbStackDepthOnEntry() const
{
    return (bbEntryState ? bbEntryState->esStackDepth : 0);
}

//------------------------------------------------------------------------
// bbSetStack: update IL stack for block entry
//
// Arguments;
//   stack - new stack for block
//
void BasicBlock::bbSetStack(StackEntry* stack)
{
    assert(bbEntryState);
    assert(stack);
    bbEntryState->esStack = stack;
}

//------------------------------------------------------------------------
// bbStackOnEntry: fetch IL stack for block entry
//
StackEntry* BasicBlock::bbStackOnEntry() const
{
    assert(bbEntryState);
    return bbEntryState->esStack;
}

//------------------------------------------------------------------------
// StatementCount: number of statements in the block.
//
// Returns:
//   count of statements
//
// Notes:
//   If you are calling this in order to compare the statement count
//   against a limit, use StatementCountExceeds as it may do less work.
//
unsigned BasicBlock::StatementCount()
{
    unsigned count = 0;

    for (Statement* const stmt : Statements())
    {
        count++;
    }

    return count;
}

//------------------------------------------------------------------------
// StatementCountExceeds: check if the number of statements in the block
//   exceeds some limit
//
// Arguments:
//    limit  - limit on the number of statements
//    count  - [out, optional] actual number of statements (if less than or equal to limit)
//
// Returns:
//   true if the number of statements is greater than limit
//
bool BasicBlock::StatementCountExceeds(unsigned limit, unsigned* count /* = nullptr */)
{
    unsigned localCount = 0;
    bool     overLimit  = false;

    for (Statement* const stmt : Statements())
    {
        if (++localCount > limit)
        {
            overLimit = true;
            break;
        }
    }

    if (count != nullptr)
    {
        *count = localCount;
    }

    return overLimit;
}
