// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "jitpch.h"

#ifdef _MSC_VER
#pragma hdrstop
#endif

#include "lower.h" // for LowerRange()

// Flowgraph Optimization

//------------------------------------------------------------------------
// fgComputeReturnBlocks: Compute the set of BBJ_RETURN blocks.
//
// Initialize `fgReturnBlocks` to a list of the BBJ_RETURN blocks in the function.
//
void Compiler::fgComputeReturnBlocks()
{
    fgReturnBlocks = nullptr;

    for (BasicBlock* const block : Blocks())
    {
        // If this is a BBJ_RETURN block, add it to our list of all BBJ_RETURN blocks. This list is only
        // used to find return blocks.
        if (block->KindIs(BBJ_RETURN))
        {
            fgReturnBlocks = new (this, CMK_Reachability) BasicBlockList(block, fgReturnBlocks);
        }
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("Return blocks:");
        if (fgReturnBlocks == nullptr)
        {
            printf(" NONE");
        }
        else
        {
            for (const BasicBlockList* bl = fgReturnBlocks; bl != nullptr; bl = bl->next)
            {
                printf(" " FMT_BB, bl->block->bbNum);
            }
        }
        printf("\n");
    }
#endif // DEBUG
}

//------------------------------------------------------------------------
// fgRemoveUnreachableBlocks: Remove unreachable blocks.
//
// Some blocks (marked with BBF_DONT_REMOVE) can't be removed even if unreachable, in which case they
// are converted to `throw` blocks. Internal throw helper blocks and the single return block (if any)
// are never considered unreachable.
//
// Arguments:
//   canRemoveBlock - Method that determines if a block can be removed or not. In earlier phases, it
//       relies on the reachability set. During final phase, it depends on the DFS walk of the flowgraph
//       and considering blocks that are not visited as unreachable.
//
// Return Value:
//    Return true if changes were made that may cause additional blocks to be removable.
//
// Notes:
//    Unreachable blocks removal phase happens twice.
//
//    During early phases RecomputeLoopInfo, the logic to determine if a block is reachable
//    or not is based on the reachability sets, and hence it must be computed and valid.
//
//    During late phase, all the reachable blocks from fgFirstBB are traversed and everything
//    else are marked as unreachable (with exceptions of handler/filter blocks). As such, it
//    is not dependent on the validity of reachability sets.
//
template <typename CanRemoveBlockBody>
bool Compiler::fgRemoveUnreachableBlocks(CanRemoveBlockBody canRemoveBlock)
{
    bool hasUnreachableBlocks = false;
    bool changed              = false;

    // Mark unreachable blocks with BBF_REMOVED.
    for (BasicBlock* const block : Blocks())
    {
        // Internal throw blocks are always reachable.
        if (fgIsThrowHlpBlk(block))
        {
            continue;
        }
        else if (block == genReturnBB)
        {
            // Don't remove statements for the genReturnBB block, as we might have special hookups there.
            // For example, the profiler hookup needs to have the "void GT_RETURN" statement
            // to properly set the info.compProfilerCallback flag.
            continue;
        }
        else if (block->HasFlag(BBF_DONT_REMOVE) && block->isEmpty() && block->KindIs(BBJ_THROW))
        {
            // We already converted a non-removable block to a throw; don't bother processing it again.
            continue;
        }
        else if (!canRemoveBlock(block))
        {
            continue;
        }

        // Remove all the code for the block
        fgUnreachableBlock(block);

        // Make sure that the block was marked as removed */
        noway_assert(block->HasFlag(BBF_REMOVED));

        if (block->HasFlag(BBF_DONT_REMOVE))
        {
            // Unmark the block as removed, clear BBF_INTERNAL, and set BBF_IMPORTED

            JITDUMP("Converting BBF_DONT_REMOVE block " FMT_BB " to BBJ_THROW\n", block->bbNum);

            // If the CALLFINALLY is being replaced by a throw, then the CALLFINALLYRET is unreachable.
            if (block->isBBCallFinallyPair())
            {
                BasicBlock* const leaveBlock = block->Next();
                fgPrepareCallFinallyRetForRemoval(leaveBlock);
            }

            // The successors may be unreachable after this change.
            changed |= block->NumSucc() > 0;

            block->RemoveFlags(BBF_REMOVED | BBF_INTERNAL);
            block->SetFlags(BBF_IMPORTED);
            block->SetKindAndTargetEdge(BBJ_THROW);
            block->bbSetRunRarely();
        }
        else
        {
            /* We have to call fgRemoveBlock next */
            hasUnreachableBlocks = true;
            changed              = true;
        }
    }

    if (hasUnreachableBlocks)
    {
        // Now remove the unreachable blocks: if we marked a block with BBF_REMOVED then we need to
        // call fgRemoveBlock() on it.
        BasicBlock* bNext;
        for (BasicBlock* block = fgFirstBB; block != nullptr; block = bNext)
        {
            if (block->HasFlag(BBF_REMOVED))
            {
                bNext = fgRemoveBlock(block, /* unreachable */ true);
            }
            else
            {
                bNext = block->Next();
            }
        }
    }

    return changed;
}

//-------------------------------------------------------------
// fgComputeDominators: Compute dominators
//
// Returns:
//    Suitable phase status.
//
PhaseStatus Compiler::fgComputeDominators()
{
    if (m_dfsTree == nullptr)
    {
        m_dfsTree = fgComputeDfs();
    }

    if (m_domTree == nullptr)
    {
        m_domTree = FlowGraphDominatorTree::Build(m_dfsTree);
    }

    bool anyHandlers = false;
    for (EHblkDsc* const HBtab : EHClauses(this))
    {
        if (HBtab->HasFilter())
        {
            BasicBlock* filter = HBtab->ebdFilter;
            if (m_dfsTree->Contains(filter))
            {
                filter->SetDominatedByExceptionalEntryFlag();
                anyHandlers = true;
            }
        }

        BasicBlock* handler = HBtab->ebdHndBeg;
        if (m_dfsTree->Contains(handler))
        {
            handler->SetDominatedByExceptionalEntryFlag();
            anyHandlers = true;
        }
    }

    if (anyHandlers)
    {
        assert(m_dfsTree->GetPostOrder(m_dfsTree->GetPostOrderCount() - 1) == fgFirstBB);
        // Now propagate dominator flag in reverse post-order, skipping first BB.
        // (This could walk the dominator tree instead, but this linear order
        // is more efficient to visit and still guarantees we see the
        // dominators before the dominated blocks).
        for (unsigned i = m_dfsTree->GetPostOrderCount() - 1; i != 0; i--)
        {
            BasicBlock* block = m_dfsTree->GetPostOrder(i - 1);
            assert(block->bbIDom != nullptr);
            if (block->bbIDom->IsDominatedByExceptionalEntryFlag())
            {
                block->SetDominatedByExceptionalEntryFlag();
            }
        }
    }

    return PhaseStatus::MODIFIED_NOTHING;
}

//-------------------------------------------------------------
// fgInitBlockVarSets: Initialize the per-block variable sets (used for liveness analysis).
//
// Notes:
//   Initializes:
//      bbVarUse, bbVarDef, bbLiveIn, bbLiveOut,
//      bbMemoryUse, bbMemoryDef, bbMemoryLiveIn, bbMemoryLiveOut,
//      bbScope
//
void Compiler::fgInitBlockVarSets()
{
    for (BasicBlock* const block : Blocks())
    {
        block->InitVarSets(this);
    }

    fgBBVarSetsInited = true;
}

//------------------------------------------------------------------------
// fgPostImportationCleanups: clean up flow graph after importation
//
// Returns:
//   suitable phase status
//
// Notes:
//
//  Find and remove any basic blocks that are useless (e.g. they have not been
//  imported because they are not reachable, or they have been optimized away).
//
//  Remove try regions where no blocks in the try were imported.
//  Update the end of try and handler regions where trailing blocks were not imported.
//  Update the start of try regions that were partially imported (OSR)
//
//  For OSR, add "step blocks" and conditional logic to ensure the path from
//  method entry to the OSR logical entry point always flows through the first
//  block of any enclosing try.
//
//  In particular, given a method like
//
//  S0;
//  try {
//      S1;
//      try {
//          S2;
//          for (...) {}  // OSR logical entry here
//      }
//  }
//
//  Where the Sn are arbitrary hammocks of code, the OSR logical entry point
//  would be in the middle of a nested try. We can't branch there directly
//  from the OSR method entry. So we transform the flow to:
//
//  _firstCall = 0;
//  goto pt1;
//  S0;
//  pt1:
//  try {
//      if (_firstCall == 0) goto pt2;
//      S1;
//      pt2:
//      try {
//          if (_firstCall == 0) goto pp;
//          S2;
//          pp:
//          _firstCall = 1;
//          for (...)
//      }
//  }
//
//  where the "state variable" _firstCall guides execution appropriately
//  from OSR method entry, and flow always enters the try blocks at the
//  first block of the try.
//
PhaseStatus Compiler::fgPostImportationCleanup()
{
    // Bail, if this is a failed inline
    //
    if (compDonotInline())
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    if (compIsForInlining())
    {
        // Update type of return spill temp if we have gathered
        // better info when importing the inlinee, and the return
        // spill temp is single def or was freshly created for this inlinee
        if (fgNeedReturnSpillTemp())
        {
            CORINFO_CLASS_HANDLE retExprClassHnd = impInlineInfo->retExprClassHnd;
            if (retExprClassHnd != nullptr)
            {
                LclVarDsc* returnSpillVarDsc = lvaGetDesc(lvaInlineeReturnSpillTemp);

                if (returnSpillVarDsc->lvType == TYP_REF &&
                    (returnSpillVarDsc->lvSingleDef || lvaInlineeReturnSpillTempFreshlyCreated))
                {
                    lvaUpdateClass(lvaInlineeReturnSpillTemp, retExprClassHnd, impInlineInfo->retExprClassHndIsExact,
                                   false);
                }
            }
        }
    }

    BasicBlock* cur;
    BasicBlock* nxt;

    // If we remove any blocks, we'll have to do additional work
    unsigned removedBlks = 0;

    for (cur = fgFirstBB; cur != nullptr; cur = nxt)
    {
        // Get hold of the next block (in case we delete 'cur')
        nxt = cur->Next();

        // Should this block be removed?
        if (!cur->HasFlag(BBF_IMPORTED))
        {
            noway_assert(cur->isEmpty());

            if (ehCanDeleteEmptyBlock(cur))
            {
                JITDUMP(FMT_BB " was not imported, marking as removed (%d)\n", cur->bbNum, removedBlks);

                // Notify all successors that cur is no longer a pred.
                //
                // This may not be necessary once we have pred lists built before importation.
                // When we alter flow in the importer branch opts, we should be able to make
                // suitable updates there for blocks that we plan to keep.
                //
                for (BasicBlock* succ : cur->Succs())
                {
                    fgRemoveAllRefPreds(succ, cur);
                }

                cur->SetFlags(BBF_REMOVED);
                removedBlks++;

                // Drop the block from the list.
                //
                // We rely on the fact that this does not clear out
                // cur->bbNext or cur->bbPrev in the code that
                // follows.
                fgUnlinkBlockForRemoval(cur);
            }
            else
            {
                // We were prevented from deleting this block by EH
                // normalization. Mark the block as imported.
                cur->SetFlags(BBF_IMPORTED);
            }
        }
    }

    // If no blocks were removed, we're done.
    // Unless we are an OSR method with a try entry.
    //
    if ((removedBlks == 0) && !(opts.IsOSR() && fgOSREntryBB->hasTryIndex()))
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    // Update all references in the exception handler table.
    //
    // We may have made the entire try block unreachable.
    // Check for this case and remove the entry from the EH table.
    //
    // For OSR, just the initial part of a try range may become
    // unreachable; if so we need to shrink the try range down
    // to the portion that was imported.
    unsigned  XTnum;
    EHblkDsc* HBtab;
    unsigned  delCnt = 0;

    // Walk the EH regions from inner to outer
    for (XTnum = 0, HBtab = compHndBBtab; XTnum < compHndBBtabCount; XTnum++, HBtab++)
    {
    AGAIN:

        // If start of a try region was not imported, then we either
        // need to trim the region extent, or remove the region
        // entirely.
        //
        // In normal importation, it is not valid to jump into the
        // middle of a try, so if the try entry was not imported, the
        // entire try can be removed.
        //
        // In OSR importation the entry patchpoint may be in the
        // middle of a try, and we need to determine how much of the
        // try ended up getting imported.  Because of backwards
        // branches we may end up importing the entire try even though
        // execution starts in the middle.
        //
        // Note it is common in both cases for the ends of trys (and
        // associated handlers) to end up not getting imported, so if
        // the try region is not removed, we always check if we need
        // to trim the ends.
        //
        if (HBtab->ebdTryBeg->HasFlag(BBF_REMOVED))
        {
            // Usual case is that the entire try can be removed.
            bool removeTryRegion = true;

            if (opts.IsOSR())
            {
                // For OSR we may need to trim the try region start.
                //
                // We rely on the fact that removed blocks have been snipped from
                // the main block list, but that those removed blocks have kept
                // their bbprev (and bbnext) links.
                //
                // Find the first unremoved block before the try entry block.
                //
                BasicBlock* const oldTryEntry  = HBtab->ebdTryBeg;
                BasicBlock*       tryEntryPrev = oldTryEntry->Prev();
                assert(tryEntryPrev != nullptr);
                while (tryEntryPrev->HasFlag(BBF_REMOVED))
                {
                    tryEntryPrev = tryEntryPrev->Prev();
                    // Because we've added an unremovable scratch block as
                    // fgFirstBB, this backwards walk should always find
                    // some block.
                    assert(tryEntryPrev != nullptr);
                }

                // If there is a next block of this prev block, and that block is
                // contained in the current try, we'd like to make that block
                // the new start of the try, and keep the region.
                BasicBlock* newTryEntry    = tryEntryPrev->Next();
                bool        updateTryEntry = false;

                if ((newTryEntry != nullptr) && bbInTryRegions(XTnum, newTryEntry))
                {
                    // We want to trim the begin extent of the current try region to newTryEntry.
                    //
                    // This method is invoked after EH normalization, so we may need to ensure all
                    // try regions begin at blocks that are not the start or end of some other try.
                    //
                    // So, see if this block is already the start or end of some other EH region.
                    if (bbIsTryBeg(newTryEntry))
                    {
                        // We've already end-trimmed the inner try. Do the same now for the
                        // current try, so it is easier to detect when they mutually protect.
                        // (we will call this again later, which is harmless).
                        fgSkipRmvdBlocks(HBtab);

                        // If this try and the inner try form a "mutually protected try region"
                        // then we must continue to share the try entry block.
                        EHblkDsc* const HBinner = ehGetBlockTryDsc(newTryEntry);
                        assert(HBinner->ebdTryBeg == newTryEntry);

                        if (HBtab->ebdTryLast != HBinner->ebdTryLast)
                        {
                            updateTryEntry = true;
                        }
                    }
                    // Also, a try and handler cannot start at the same block
                    else if (bbIsHandlerBeg(newTryEntry))
                    {
                        updateTryEntry = true;
                    }

                    if (updateTryEntry)
                    {
                        // We need to trim the current try to begin at a different block. Normally
                        // this would be problematic as we don't have enough context to redirect
                        // all the incoming edges, but we know oldTryEntry is unreachable.
                        // So there are no incoming edges to worry about.
                        // What follows is similar to fgNewBBInRegion, but we can't call that
                        // here as the oldTryEntry is no longer in the main bb list.
                        newTryEntry = BasicBlock::New(this);
                        newTryEntry->SetFlags(BBF_IMPORTED | BBF_INTERNAL);
                        newTryEntry->bbRefs = 0;

                        // Set the right EH region indices on this new block.
                        //
                        // Patchpoints currently cannot be inside handler regions,
                        // and so likewise the old and new try region entries.
                        assert(!oldTryEntry->hasHndIndex());
                        newTryEntry->setTryIndex(XTnum);
                        newTryEntry->clearHndIndex();
                        fgInsertBBafter(tryEntryPrev, newTryEntry);

                        // Generally this (unreachable) empty new try entry block can fall through
                        // to the next block, but in cases where there's a nested try with an
                        // out of order handler, the next block may be a handler. So even though
                        // this new try entry block is unreachable, we need to give it a
                        // plausible flow target. Simplest is to just mark it as a throw.
                        if (bbIsHandlerBeg(newTryEntry->Next()))
                        {
                            newTryEntry->SetKindAndTargetEdge(BBJ_THROW);
                        }
                        else
                        {
                            FlowEdge* const newEdge = fgAddRefPred(newTryEntry->Next(), newTryEntry);
                            newTryEntry->SetKindAndTargetEdge(BBJ_ALWAYS, newEdge);
                        }

                        JITDUMP("OSR: changing start of try region #%u from " FMT_BB " to new " FMT_BB "\n",
                                XTnum + delCnt, oldTryEntry->bbNum, newTryEntry->bbNum);
                    }
                    else
                    {
                        // We can just trim the try to newTryEntry as it is not part of some inner try or handler.
                        JITDUMP("OSR: changing start of try region #%u from " FMT_BB " to " FMT_BB "\n", XTnum + delCnt,
                                oldTryEntry->bbNum, newTryEntry->bbNum);
                    }

                    // Update the handler table
                    fgSetTryBeg(HBtab, newTryEntry);

                    // Try entry blocks get specially marked and have special protection.
                    HBtab->ebdTryBeg->SetFlags(BBF_DONT_REMOVE);

                    // We are keeping this try region
                    removeTryRegion = false;
                }
            }

            if (removeTryRegion)
            {
                // In the dump, refer to the region by its original index.
                JITDUMP("Try region #%u (" FMT_BB " -- " FMT_BB ") not imported, removing try from the EH table\n",
                        XTnum + delCnt, HBtab->ebdTryBeg->bbNum, HBtab->ebdTryLast->bbNum);

                delCnt++;

                fgRemoveEHTableEntry(XTnum);

                if (XTnum < compHndBBtabCount)
                {
                    // There are more entries left to process, so do more. Note that
                    // HBtab now points to the next entry, that we copied down to the
                    // current slot. XTnum also stays the same.
                    goto AGAIN;
                }

                // no more entries (we deleted the last one), so exit the loop
                break;
            }
        }

        // If we get here, the try entry block was not removed.
        // Check some invariants.
        assert(HBtab->ebdTryBeg->HasFlag(BBF_IMPORTED));
        assert(HBtab->ebdTryBeg->HasFlag(BBF_DONT_REMOVE));
        assert(HBtab->ebdHndBeg->HasFlag(BBF_IMPORTED));
        assert(HBtab->ebdHndBeg->HasFlag(BBF_DONT_REMOVE));

        if (HBtab->HasFilter())
        {
            assert(HBtab->ebdFilter->HasFlag(BBF_IMPORTED));
            assert(HBtab->ebdFilter->HasFlag(BBF_DONT_REMOVE));
        }

        // Finally, do region end trimming -- update try and handler ends to reflect removed blocks.
        fgSkipRmvdBlocks(HBtab);
    }

    // If this is OSR, and the OSR entry was mid-try or in a nested try entry,
    // add the appropriate step block logic.
    //
    unsigned addedBlocks = 0;
    bool     addedTemps  = 0;

    if (opts.IsOSR())
    {
        BasicBlock* const osrEntry        = fgOSREntryBB;
        BasicBlock*       entryJumpTarget = osrEntry;

        if (osrEntry->hasTryIndex())
        {
            EHblkDsc*   enclosingTry   = ehGetBlockTryDsc(osrEntry);
            BasicBlock* tryEntry       = enclosingTry->ebdTryBeg;
            bool const  inNestedTry    = (enclosingTry->ebdEnclosingTryIndex != EHblkDsc::NO_ENCLOSING_INDEX);
            bool const  osrEntryMidTry = (osrEntry != tryEntry);

            if (inNestedTry || osrEntryMidTry)
            {
                JITDUMP("OSR Entry point at IL offset 0x%0x (" FMT_BB ") is %s%s try region EH#%u\n", info.compILEntry,
                        osrEntry->bbNum, osrEntryMidTry ? "within " : "at the start of ", inNestedTry ? "nested" : "",
                        osrEntry->getTryIndex());

                // We'll need a state variable to control the branching.
                //
                // It will be initialized to zero when the OSR method is entered and set to one
                // once flow reaches the osrEntry.
                //
                unsigned const entryStateVar   = lvaGrabTemp(false DEBUGARG("OSR entry state var"));
                lvaTable[entryStateVar].lvType = TYP_INT;
                addedTemps                     = true;

                // Zero the entry state at method entry.
                //
                GenTree* const initEntryState = gtNewTempStore(entryStateVar, gtNewZeroConNode(TYP_INT));
                fgNewStmtAtBeg(fgFirstBB, initEntryState);

                // Set the state variable once control flow reaches the OSR entry.
                //
                GenTree* const setEntryState = gtNewTempStore(entryStateVar, gtNewOneConNode(TYP_INT));
                fgNewStmtAtBeg(osrEntry, setEntryState);

                // Helper method to add flow
                //
                auto addConditionalFlow = [this, entryStateVar, &entryJumpTarget, &addedBlocks](BasicBlock* fromBlock,
                                                                                                BasicBlock* toBlock) {
                    BasicBlock* const newBlock = fgSplitBlockAtBeginning(fromBlock);
                    newBlock->inheritWeight(fromBlock);
                    fromBlock->SetFlags(BBF_INTERNAL);
                    newBlock->RemoveFlags(BBF_DONT_REMOVE);
                    addedBlocks++;
                    FlowEdge* const normalTryEntryEdge = fromBlock->GetTargetEdge();

                    GenTree* const entryStateLcl = gtNewLclvNode(entryStateVar, TYP_INT);
                    GenTree* const compareEntryStateToZero =
                        gtNewOperNode(GT_EQ, TYP_INT, entryStateLcl, gtNewZeroConNode(TYP_INT));
                    GenTree* const jumpIfEntryStateZero = gtNewOperNode(GT_JTRUE, TYP_VOID, compareEntryStateToZero);
                    fgNewStmtAtBeg(fromBlock, jumpIfEntryStateZero);

                    FlowEdge* const osrTryEntryEdge = fgAddRefPred(toBlock, fromBlock);
                    fromBlock->SetCond(osrTryEntryEdge, normalTryEntryEdge);

                    if (fgHaveProfileWeights())
                    {
                        // We are adding a path from (ultimately) the method entry to "fromBlock"
                        // Update the profile weight.
                        //
                        weight_t const entryWeight = fgFirstBB->bbWeight;

                        JITDUMP("Updating block weight for now-reachable try entry " FMT_BB " via " FMT_BB "\n",
                                fromBlock->bbNum, fgFirstBB->bbNum);
                        fromBlock->increaseBBProfileWeight(entryWeight);

                        // We updated the weight of fromBlock above.
                        //
                        // Set the likelihoods such that the additional weight flows to toBlock
                        // (and so the "normal path" profile out of fromBlock to newBlock is unaltered)
                        //
                        // In some stress cases we may have a zero-weight OSR entry.
                        // Tolerate this by capping the fromToLikelihood.
                        //
                        weight_t const fromWeight       = fromBlock->bbWeight;
                        weight_t const fromToLikelihood = min(1.0, entryWeight / fromWeight);

                        osrTryEntryEdge->setLikelihood(fromToLikelihood);
                        normalTryEntryEdge->setLikelihood(1.0 - fromToLikelihood);
                    }
                    else
                    {
                        // Just set likelihoods arbitrarily
                        //
                        osrTryEntryEdge->setLikelihood(0.9);
                        normalTryEntryEdge->setLikelihood(0.1);
                    }

                    entryJumpTarget = fromBlock;
                };

                // If this is a mid-try entry, add a conditional branch from the start of the try to osr entry point.
                //
                if (osrEntryMidTry)
                {
                    addConditionalFlow(tryEntry, osrEntry);
                }

                // Add conditional branches for each successive enclosing try with a distinct
                // entry block.
                //
                while (enclosingTry->ebdEnclosingTryIndex != EHblkDsc::NO_ENCLOSING_INDEX)
                {
                    EHblkDsc* const   nextTry      = ehGetDsc(enclosingTry->ebdEnclosingTryIndex);
                    BasicBlock* const nextTryEntry = nextTry->ebdTryBeg;

                    // We don't need to add flow for mutual-protect regions
                    // (multiple tries that all share the same entry block).
                    //
                    if (nextTryEntry != tryEntry)
                    {
                        addConditionalFlow(nextTryEntry, tryEntry);
                    }
                    enclosingTry = nextTry;
                    tryEntry     = nextTryEntry;
                }

                // Transform the method entry flow, if necessary.
                //
                // Note even if the OSR is in a nested try, if it's a mutual protect try
                // it can be reached directly from "outside".
                //
                assert(fgFirstBB->TargetIs(osrEntry));
                assert(fgFirstBB->KindIs(BBJ_ALWAYS));

                if (entryJumpTarget != osrEntry)
                {
                    fgRedirectEdge(fgFirstBB->TargetEdgeRef(), entryJumpTarget);

                    JITDUMP("OSR: redirecting flow from method entry " FMT_BB " to OSR entry " FMT_BB
                            " via step blocks.\n",
                            fgFirstBB->bbNum, fgOSREntryBB->bbNum);
                }
                else
                {
                    JITDUMP("OSR: leaving direct flow from method entry " FMT_BB " to OSR entry " FMT_BB
                            ", no step blocks needed.\n",
                            fgFirstBB->bbNum, fgOSREntryBB->bbNum);
                }
            }
            else
            {
                // If OSR entry is the start of an un-nested try, no work needed.
                //
                // We won't hit this case today as we don't allow the try entry to be the target of a backedge,
                // and currently patchpoints only appear at targets of backedges.
                //
                JITDUMP("OSR Entry point at IL offset 0x%0x (" FMT_BB
                        ") is start of an un-nested try region, no step blocks needed.\n",
                        info.compILEntry, osrEntry->bbNum);
                assert(entryJumpTarget == osrEntry);
                assert(fgOSREntryBB == osrEntry);
            }
        }
        else
        {
            // If OSR entry is not within a try, no work needed.
            //
            JITDUMP("OSR Entry point at IL offset 0x%0x (" FMT_BB ") is not in a try region, no step blocks needed.\n",
                    info.compILEntry, osrEntry->bbNum);
            assert(entryJumpTarget == osrEntry);
            assert(fgOSREntryBB == osrEntry);
        }
    }

#ifdef DEBUG
    fgVerifyHandlerTab();
#endif // DEBUG

    // Did we make any changes?
    //
    const bool madeChanges = (addedBlocks > 0) || (delCnt > 0) || (removedBlks > 0) || addedTemps;

    // Note that we have now run post importation cleanup,
    // so we can enable more stringent checking.
    //
    compPostImportationCleanupDone = true;

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//-------------------------------------------------------------
// fgCanCompactBlock: Determine if a BBJ_ALWAYS block and its target can be compacted.
//
// Arguments:
//    block - BBJ_ALWAYS block to check
//
// Returns:
//    true if compaction is allowed
//
bool Compiler::fgCanCompactBlock(BasicBlock* block)
{
    assert(block != nullptr);

    if (!block->KindIs(BBJ_ALWAYS) || block->HasFlag(BBF_KEEP_BBJ_ALWAYS))
    {
        return false;
    }

    BasicBlock* const target = block->GetTarget();

    if (block == target)
    {
        return false;
    }

    if (target->IsFirst() || (target == fgEntryBB) || (target == fgOSREntryBB))
    {
        return false;
    }

    // Don't bother compacting a call-finally pair if it doesn't succeed block
    //
    if (target->isBBCallFinallyPair() && !block->NextIs(target))
    {
        return false;
    }

    // If target has multiple incoming edges, we can still compact if block is empty.
    // However, not if it is the beginning of a handler.
    //
    if (target->countOfInEdges() != 1 && (!block->isEmpty() || (block->bbCatchTyp != BBCT_NONE)))
    {
        return false;
    }

    if (target->HasFlag(BBF_DONT_REMOVE))
    {
        return false;
    }

    // Ensure we leave a valid init BB around.
    //
    if ((block == fgFirstBB) && !fgCanCompactInitBlock())
    {
        return false;
    }

    // We cannot compact two blocks in different EH regions.
    //
    if (!BasicBlock::sameEHRegion(block, target))
    {
        return false;
    }

    return true;
}

//-------------------------------------------------------------
// fgCanCompactInitBlock: Check if the first BB (the init BB) can be compacted
// into its target.
//
// Returns:
//    true if compaction is allowed
//
bool Compiler::fgCanCompactInitBlock()
{
    assert(fgFirstBB->KindIs(BBJ_ALWAYS));
    BasicBlock* target = fgFirstBB->GetTarget();
    if (target->hasTryIndex())
    {
        // Inside a try region
        return false;
    }

    assert(target->bbPreds != nullptr);
    if (target->bbPreds->getNextPredEdge() != nullptr)
    {
        // Multiple preds
        return false;
    }

    if (opts.compDbgCode && !target->HasFlag(BBF_INTERNAL))
    {
        // Init BB must be internal for debug code to avoid conflating
        // JIT-inserted code with user code.
        return false;
    }

    return true;
}

//-------------------------------------------------------------
// fgCompactBlock: Compact BBJ_ALWAYS block and its target into one.
//
// Requires that all necessary checks have been performed, i.e. fgCanCompactBlock returns true.
//
// Uses for this function - whenever we change links, insert blocks, ...
// It will keep the flowgraph data in synch - bbNum, bbRefs, bbPreds
//
// Arguments:
//    block - move all code into this block from its target.
//
void Compiler::fgCompactBlock(BasicBlock* block)
{
    assert(fgCanCompactBlock(block));

    // We shouldn't churn the flowgraph after doing hot/cold splitting
    assert(fgFirstColdBlock == nullptr);

    BasicBlock* const target = block->GetTarget();

    JITDUMP("\nCompacting " FMT_BB " into " FMT_BB ":\n", target->bbNum, block->bbNum);
    fgRemoveRefPred(block->GetTargetEdge());

    if (target->countOfInEdges() > 0)
    {
        JITDUMP("Second block has %u other incoming edges\n", target->countOfInEdges());
        assert(block->isEmpty());

        // Retarget all the other edges incident on target
        for (BasicBlock* const predBlock : target->PredBlocksEditing())
        {
            fgReplaceJumpTarget(predBlock, target, block);
        }
    }

    assert(target->countOfInEdges() == 0);
    assert(target->bbPreds == nullptr);

    /* Start compacting - move all the statements in the second block to the first block */

    // First move any phi definitions of the second block after the phi defs of the first.
    // TODO-CQ: This may be the wrong thing to do. If we're compacting blocks, it's because a
    // control-flow choice was constant-folded away. So probably phi's need to go away,
    // as well, in favor of one of the incoming branches. Or at least be modified.

    assert(block->IsLIR() == target->IsLIR());
    if (block->IsLIR())
    {
        LIR::Range& blockRange  = LIR::AsRange(block);
        LIR::Range& targetRange = LIR::AsRange(target);

        // Does target have any phis?
        GenTree* targetNode = targetRange.FirstNode();

        // Does the block have any code?
        if (targetNode != nullptr)
        {
            LIR::Range targetNodes = targetRange.Remove(targetNode, targetRange.LastNode());
            blockRange.InsertAtEnd(std::move(targetNodes));
        }
    }
    else
    {
        Statement* blkNonPhi1    = block->FirstNonPhiDef();
        Statement* targetNonPhi1 = target->FirstNonPhiDef();
        Statement* blkFirst      = block->firstStmt();
        Statement* targetFirst   = target->firstStmt();

        // Does the second have any phis?
        if ((targetFirst != nullptr) && (targetFirst != targetNonPhi1))
        {
            Statement* targetLast = targetFirst->GetPrevStmt();
            assert(targetLast->GetNextStmt() == nullptr);

            // Does "blk" have phis?
            if (blkNonPhi1 != blkFirst)
            {
                // Yes, has phis.
                // Insert after the last phi of "block."
                // First, targetPhis after last phi of block.
                Statement* blkLastPhi = (blkNonPhi1 != nullptr) ? blkNonPhi1->GetPrevStmt() : blkFirst->GetPrevStmt();
                blkLastPhi->SetNextStmt(targetFirst);
                targetFirst->SetPrevStmt(blkLastPhi);

                // Now, rest of "block" after last phi of "target".
                Statement* targetLastPhi =
                    (targetNonPhi1 != nullptr) ? targetNonPhi1->GetPrevStmt() : targetFirst->GetPrevStmt();
                targetLastPhi->SetNextStmt(blkNonPhi1);

                if (blkNonPhi1 != nullptr)
                {
                    blkNonPhi1->SetPrevStmt(targetLastPhi);
                }
                else
                {
                    // block has no non phis, so make the last statement be the last added phi.
                    blkFirst->SetPrevStmt(targetLastPhi);
                }

                // Now update the bbStmtList of "target".
                target->bbStmtList = targetNonPhi1;
                if (targetNonPhi1 != nullptr)
                {
                    targetNonPhi1->SetPrevStmt(targetLast);
                }
            }
            else
            {
                if (blkFirst != nullptr) // If "block" has no statements, fusion will work fine...
                {
                    // First, targetPhis at start of block.
                    Statement* blkLast = blkFirst->GetPrevStmt();
                    block->bbStmtList  = targetFirst;
                    // Now, rest of "block" (if it exists) after last phi of "target".
                    Statement* targetLastPhi =
                        (targetNonPhi1 != nullptr) ? targetNonPhi1->GetPrevStmt() : targetFirst->GetPrevStmt();

                    targetFirst->SetPrevStmt(blkLast);
                    targetLastPhi->SetNextStmt(blkFirst);
                    blkFirst->SetPrevStmt(targetLastPhi);
                    // Now update the bbStmtList of "target"
                    target->bbStmtList = targetNonPhi1;
                    if (targetNonPhi1 != nullptr)
                    {
                        targetNonPhi1->SetPrevStmt(targetLast);
                    }
                }
            }
        }

        // Now proceed with the updated bbTreeLists.
        Statement* stmtList1 = block->firstStmt();
        Statement* stmtList2 = target->firstStmt();

        /* the block may have an empty list */

        if (stmtList1 != nullptr)
        {
            Statement* stmtLast1 = block->lastStmt();

            /* The second block may be a GOTO statement or something with an empty bbStmtList */
            if (stmtList2 != nullptr)
            {
                Statement* stmtLast2 = target->lastStmt();

                /* append list2 to list 1 */

                stmtLast1->SetNextStmt(stmtList2);
                stmtList2->SetPrevStmt(stmtLast1);
                stmtList1->SetPrevStmt(stmtLast2);
            }
        }
        else
        {
            /* block was formerly empty and now has target's statements */
            block->bbStmtList = stmtList2;
        }
    }

    // Transfer target's weight to block
    // (target's weight should include block's weight,
    // plus the weights of target's preds, which now flow into block)
    const bool hasProfileWeight = block->hasProfileWeight();
    block->inheritWeight(target);

    if (hasProfileWeight)
    {
        block->SetFlags(BBF_PROF_WEIGHT);
    }

    VarSetOps::AssignAllowUninitRhs(this, block->bbLiveOut, target->bbLiveOut);

    // Update the beginning and ending IL offsets (bbCodeOffs and bbCodeOffsEnd).
    // Set the beginning IL offset to the minimum, and the ending offset to the maximum, of the respective blocks.
    // If one block has an unknown offset, we take the other block.
    // We are merging into 'block', so if its values are correct, just leave them alone.
    // TODO: we should probably base this on the statements within.

    if (block->bbCodeOffs == BAD_IL_OFFSET)
    {
        block->bbCodeOffs = target->bbCodeOffs; // If they are both BAD_IL_OFFSET, this doesn't change anything.
    }
    else if (target->bbCodeOffs != BAD_IL_OFFSET)
    {
        // The are both valid offsets; compare them.
        if (block->bbCodeOffs > target->bbCodeOffs)
        {
            block->bbCodeOffs = target->bbCodeOffs;
        }
    }

    if (block->bbCodeOffsEnd == BAD_IL_OFFSET)
    {
        block->bbCodeOffsEnd = target->bbCodeOffsEnd; // If they are both BAD_IL_OFFSET, this doesn't change anything.
    }
    else if (target->bbCodeOffsEnd != BAD_IL_OFFSET)
    {
        // The are both valid offsets; compare them.
        if (block->bbCodeOffsEnd < target->bbCodeOffsEnd)
        {
            block->bbCodeOffsEnd = target->bbCodeOffsEnd;
        }
    }

    if (block->HasFlag(BBF_INTERNAL) && !target->HasFlag(BBF_INTERNAL))
    {
        // If 'block' is an internal block and 'target' isn't, then adjust the flags set on 'block'.
        block->RemoveFlags(BBF_INTERNAL); // Clear the BBF_INTERNAL flag
        block->SetFlags(BBF_IMPORTED);    // Set the BBF_IMPORTED flag
    }

    /* Update the flags for block with those found in target */

    block->CopyFlags(target, BBF_COMPACT_UPD);

    /* mark target as removed */

    target->SetFlags(BBF_REMOVED);

    /* Unlink target and update all the marker pointers if necessary */

    fgUnlinkRange(target, target);

    fgBBcount--;

    // If target was the last block of a try or handler, update the EH table.

    ehUpdateForDeletedBlock(target);

    /* Set the jump targets */

    switch (target->GetKind())
    {
        case BBJ_CALLFINALLY:
            // Propagate RETLESS property
            block->CopyFlags(target, BBF_RETLESS_CALL);

            FALLTHROUGH;

        case BBJ_ALWAYS:
        case BBJ_EHCATCHRET:
        case BBJ_EHFILTERRET:
        {
            /* Update the predecessor list for target's target */
            FlowEdge* const targetEdge = target->GetTargetEdge();
            fgReplacePred(targetEdge, block);

            block->SetKindAndTargetEdge(target->GetKind(), targetEdge);
            break;
        }

        case BBJ_COND:
        {
            /* Update the predecessor list for target's true target */
            FlowEdge* const trueEdge  = target->GetTrueEdge();
            FlowEdge* const falseEdge = target->GetFalseEdge();
            fgReplacePred(trueEdge, block);

            /* Update the predecessor list for target's false target if it is different from the true target */
            if (trueEdge != falseEdge)
            {
                fgReplacePred(falseEdge, block);
            }

            block->SetCond(trueEdge, falseEdge);
            break;
        }

        case BBJ_EHFINALLYRET:
            block->SetEhf(target->GetEhfTargets());
            fgChangeEhfBlock(target, block);
            break;

        case BBJ_EHFAULTRET:
        case BBJ_THROW:
        case BBJ_RETURN:
            /* no jumps or fall through blocks to set here */
            block->SetKind(target->GetKind());
            break;

        case BBJ_SWITCH:
            block->SetSwitch(target->GetSwitchTargets());
            // We are moving the switch jump from target to block. Examine the jump targets
            // of the BBJ_SWITCH at target and replace the predecessor to 'target' with ones to 'block'
            fgChangeSwitchBlock(target, block);
            break;

        default:
            noway_assert(!"Unexpected bbKind");
            break;
    }

    assert(block->KindIs(target->GetKind()));

#if DEBUG
    if (verbose && 0)
    {
        printf("\nAfter compacting:\n");
        fgDispBasicBlocks(false);
    }

    if (JitConfig.JitSlowDebugChecksEnabled() != 0)
    {
        // Make sure that the predecessor lists are accurate
        fgDebugCheckBBlist();
    }
#endif // DEBUG
}

//-------------------------------------------------------------
// fgUnreachableBlock: Remove a block when it is unreachable.
//
// This function cannot remove the first block.
//
// Arguments:
//    block - unreachable block to remove
//
void Compiler::fgUnreachableBlock(BasicBlock* block)
{
    // genReturnBB should never be removed, as we might have special hookups there.
    // Therefore, we should never come here to remove the statements in the genReturnBB block.
    // For example, the profiler hookup needs to have the "void GT_RETURN" statement
    // to properly set the info.compProfilerCallback flag.
    noway_assert(block != genReturnBB);

    if (block->HasFlag(BBF_REMOVED))
    {
        return;
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("\nRemoving unreachable " FMT_BB "\n", block->bbNum);
    }
#endif // DEBUG

    noway_assert(!block->IsFirst()); // Can't use this function to remove the first block

    // First, delete all the code in the block.

    if (block->IsLIR())
    {
        LIR::Range& blockRange = LIR::AsRange(block);
        if (!blockRange.IsEmpty())
        {
            blockRange.Delete(this, block, blockRange.FirstNode(), blockRange.LastNode());
        }
    }
    else
    {
        // TODO-Cleanup: I'm not sure why this happens -- if the block is unreachable, why does it have phis?
        // Anyway, remove any phis.

        Statement* firstNonPhi = block->FirstNonPhiDef();
        if (block->bbStmtList != firstNonPhi)
        {
            if (firstNonPhi != nullptr)
            {
                firstNonPhi->SetPrevStmt(block->lastStmt());
            }
            block->bbStmtList = firstNonPhi;
        }

        for (Statement* const stmt : block->Statements())
        {
            fgRemoveStmt(block, stmt);
        }
        noway_assert(block->bbStmtList == nullptr);
    }

    // Mark the block as removed
    block->SetFlags(BBF_REMOVED);

    // Update bbRefs and bbPreds for this block's successors
    bool profileInconsistent = false;
    for (BasicBlock* const succBlock : block->Succs())
    {
        FlowEdge* const succEdge = fgRemoveAllRefPreds(succBlock, block);

        if (block->hasProfileWeight() && succBlock->hasProfileWeight())
        {
            succBlock->decreaseBBProfileWeight(succEdge->getLikelyWeight());
            profileInconsistent |= (succBlock->NumSucc() > 0);
        }
    }

    if (profileInconsistent)
    {
        JITDUMP("Flow removal of " FMT_BB " needs to be propagated. Data %s inconsistent.\n", block->bbNum,
                fgPgoConsistent ? "is now" : "was already");
        fgPgoConsistent = false;
    }
}

//-------------------------------------------------------------
// fgOptimizeBranchToEmptyUnconditional:
//    Optimize a jump to an empty block which ends in an unconditional branch.
//
// Arguments:
//    block - source block
//    bDest - destination
//
// Returns: true if changes were made
//
bool Compiler::fgOptimizeBranchToEmptyUnconditional(BasicBlock* block, BasicBlock* bDest)
{
    bool optimizeJump = true;

    assert(bDest->isEmpty());
    assert(bDest->KindIs(BBJ_ALWAYS));

    BasicBlock* const bDestTarget = bDest->GetTarget();

    // Don't redirect 'block' to 'bDestTarget' if the latter jumps to 'bDest'.
    // This will lead the JIT to consider optimizing 'block' -> 'bDestTarget' -> 'bDest',
    // entering an infinite loop.
    //
    if (bDestTarget->GetUniqueSucc() == bDest)
    {
        optimizeJump = false;
    }

    // We do not optimize jumps between two different try regions.
    // However jumping to a block that is not in any try region is OK
    //
    if (bDest->hasTryIndex() && !BasicBlock::sameTryRegion(block, bDest))
    {
        optimizeJump = false;
    }

    // Don't optimize a jump to a removed block
    if (bDestTarget->HasFlag(BBF_REMOVED))
    {
        optimizeJump = false;
    }

    // Don't optimize a jump to a cloned finally
    if (bDest->HasFlag(BBF_CLONED_FINALLY_BEGIN))
    {
        optimizeJump = false;
    }

    // Must optimize jump if bDest has been removed
    //
    if (bDest->HasFlag(BBF_REMOVED))
    {
        optimizeJump = true;
    }

    if (optimizeJump)
    {
#ifdef DEBUG
        if (verbose)
        {
            printf("\nOptimizing a jump to an unconditional jump (" FMT_BB " -> " FMT_BB " -> " FMT_BB ")\n",
                   block->bbNum, bDest->bbNum, bDest->GetTarget()->bbNum);
        }
#endif // DEBUG

        weight_t removedWeight;

        // Optimize the JUMP to empty unconditional JUMP to go to the new target
        switch (block->GetKind())
        {
            case BBJ_ALWAYS:
            case BBJ_CALLFINALLYRET:
            {
                removedWeight = block->bbWeight;
                fgRedirectEdge(block->TargetEdgeRef(), bDest->GetTarget());
                break;
            }

            case BBJ_COND:
                if (block->TrueTargetIs(bDest))
                {
                    assert(!block->FalseTargetIs(bDest));
                    removedWeight = block->GetTrueEdge()->getLikelyWeight();
                    fgRedirectEdge(block->TrueEdgeRef(), bDest->GetTarget());
                }
                else
                {
                    assert(block->FalseTargetIs(bDest));
                    removedWeight = block->GetFalseEdge()->getLikelyWeight();
                    fgRedirectEdge(block->FalseEdgeRef(), bDest->GetTarget());
                }
                break;

            default:
                unreached();
        }

        //
        // When we optimize a branch to branch we need to update the profile weight
        // of bDest by subtracting out the weight of the path that is being optimized.
        //
        if (bDest->hasProfileWeight())
        {
            bDest->decreaseBBProfileWeight(removedWeight);
        }

        return true;
    }
    return false;
}

//-------------------------------------------------------------
// fgOptimizeEmptyBlock:
//   Does flow optimization of an empty block (can remove it in some cases)
//
// Arguments:
//    block - an empty block
//
// Returns: true if changes were made
//
bool Compiler::fgOptimizeEmptyBlock(BasicBlock* block)
{
    assert(block->isEmpty());

    // We shouldn't churn the flowgraph after doing hot/cold splitting
    assert(fgFirstColdBlock == nullptr);

    bool        madeChanges = false;
    BasicBlock* bPrev       = block->Prev();

    switch (block->GetKind())
    {
        case BBJ_COND:
        case BBJ_SWITCH:

            /* can never happen */
            noway_assert(!"Conditional or switch block with empty body!");
            break;

        case BBJ_THROW:
        case BBJ_CALLFINALLY:
        case BBJ_CALLFINALLYRET:
        case BBJ_RETURN:
        case BBJ_EHCATCHRET:
        case BBJ_EHFINALLYRET:
        case BBJ_EHFAULTRET:
        case BBJ_EHFILTERRET:

            /* leave them as is */
            /* some compilers generate multiple returns and put all of them at the end -
             * to solve that we need the predecessor list */

            break;

        case BBJ_ALWAYS:

            /* Special case for first BB */
            if (bPrev == nullptr)
            {
                assert(block == fgFirstBB);
                if (!block->JumpsToNext() || !fgCanCompactInitBlock())
                {
                    break;
                }
            }

            /* Do not remove a block that jumps to itself - used for while (true){} */
            if (block->TargetIs(block))
            {
                break;
            }

            // Don't remove the init BB if it does not leave a proper init BB
            // in place
            if ((block == fgFirstBB) && !fgCanCompactInitBlock())
            {
                break;
            }

            // Don't remove the fgEntryBB
            //
            if (opts.IsOSR() && (block == fgEntryBB))
            {
                break;
            }

            /* Don't remove an empty block that is in a different EH region
             * from its successor block, if the block is the target of a
             * catch return. It is required that the return address of a
             * catch be in the correct EH region, for re-raise of thread
             * abort exceptions to work. Insert a NOP in the empty block
             * to ensure we generate code for the block, if we keep it.
             */
            if (UsesFunclets())
            {
                BasicBlock* succBlock = block->GetTarget();

                if ((succBlock != nullptr) && !BasicBlock::sameEHRegion(block, succBlock))
                {
                    // The empty block and the block that follows it are in different
                    // EH regions. Is this a case where they can't be merged?

                    bool okToMerge = true; // assume it's ok
                    for (BasicBlock* const predBlock : block->PredBlocks())
                    {
                        if (predBlock->KindIs(BBJ_EHCATCHRET))
                        {
                            assert(predBlock->TargetIs(block));
                            okToMerge = false; // we can't get rid of the empty block
                            break;
                        }
                    }

                    if (!okToMerge)
                    {
                        // Insert a NOP in the empty block to ensure we generate code
                        // for the catchret target in the right EH region.
                        GenTree* nop = new (this, GT_NO_OP) GenTree(GT_NO_OP, TYP_VOID);

                        if (block->IsLIR())
                        {
                            LIR::AsRange(block).InsertAtEnd(nop);
                            LIR::ReadOnlyRange range(nop, nop);
                            m_pLowering->LowerRange(block, range);
                        }
                        else
                        {
                            Statement* nopStmt = fgNewStmtAtEnd(block, nop);
                            if (fgNodeThreading == NodeThreading::AllTrees)
                            {
                                fgSetStmtSeq(nopStmt);
                            }
                            gtSetStmtInfo(nopStmt);
                        }

                        madeChanges = true;

#ifdef DEBUG
                        if (verbose)
                        {
                            printf("\nKeeping empty block " FMT_BB " - it is the target of a catch return\n",
                                   block->bbNum);
                        }
#endif // DEBUG

                        break; // go to the next block
                    }
                }
            }

            if (!ehCanDeleteEmptyBlock(block))
            {
                // We're not allowed to remove this block due to reasons related to the EH table.
                break;
            }

            /* special case if this is the only BB */
            if (block->IsFirst() && block->IsLast())
            {
                assert(block == fgFirstBB);
                assert(block == fgLastBB);
                assert(bPrev == nullptr);
                break;
            }

            // When using profile weights, fgComputeCalledCount expects the first non-internal block to have profile
            // weight.
            // Make sure we don't break that invariant.
            if (fgIsUsingProfileWeights() && block->hasProfileWeight() && !block->HasFlag(BBF_INTERNAL))
            {
                BasicBlock* bNext = block->Next();

                // Check if the next block can't maintain the invariant.
                if ((bNext == nullptr) || bNext->HasFlag(BBF_INTERNAL) || !bNext->hasProfileWeight())
                {
                    // Check if the current block is the first non-internal block.
                    BasicBlock* curBB = bPrev;
                    while ((curBB != nullptr) && curBB->HasFlag(BBF_INTERNAL))
                    {
                        curBB = curBB->Prev();
                    }
                    if (curBB == nullptr)
                    {
                        // This block is the first non-internal block and it has profile weight.
                        // Don't delete it.
                        break;
                    }
                }
            }

            /* Remove the block */
            compCurBB = block;
            fgRemoveBlock(block, /* unreachable */ false);
            madeChanges = true;
            break;

        default:
            noway_assert(!"Unexpected bbKind");
            break;
    }

    return madeChanges;
}

//-------------------------------------------------------------
// fgOptimizeSwitchBranches:
//   Does flow optimization for a switch - bypasses jumps to empty unconditional branches,
//   and transforms degenerate switch cases like those with 1 or 2 targets.
//
// Arguments:
//    block - block with switch
//
// Returns: true if changes were made
//
bool Compiler::fgOptimizeSwitchBranches(BasicBlock* block)
{
    assert(block->KindIs(BBJ_SWITCH));

    bool modified = false;

    for (unsigned i = 0; i < block->GetSwitchTargets()->GetSuccCount(); i++)
    {
        FlowEdge* const   edge     = block->GetSwitchTargets()->GetSucc(i);
        BasicBlock* const bDest    = edge->getDestinationBlock();
        BasicBlock*       bNewDest = bDest;

        // Do we have a JUMP to an empty unconditional JUMP block?
        if (bDest->isEmpty() && bDest->KindIs(BBJ_ALWAYS) && !bDest->TargetIs(bDest)) // special case for self jumps
        {
            bool optimizeJump = true;

            // We do not optimize jumps between two different try regions.
            // However jumping to a block that is not in any try region is OK
            //
            if (bDest->hasTryIndex() && !BasicBlock::sameTryRegion(block, bDest))
            {
                optimizeJump = false;
            }

            if (optimizeJump)
            {
                bNewDest = bDest->GetTarget();
                JITDUMP("\nOptimizing a switch jump to an empty block with an unconditional jump (" FMT_BB " -> " FMT_BB
                        " -> " FMT_BB ")\n",
                        block->bbNum, bDest->bbNum, bNewDest->bbNum);
            }
        }

        if (bNewDest != bDest)
        {
            // Remove the flow into the old destination block
            //
            if (bDest->hasProfileWeight())
            {
                bDest->decreaseBBProfileWeight(edge->getLikelyWeight());
            }

            // Redirect the jump to the new target
            //
            fgReplaceJumpTarget(block, bDest, bNewDest);
            modified = true;

            // Try optimizing this edge again
            //
            i--;
        }
    }

    if (modified)
    {
        JITDUMP(
            "fgOptimizeSwitchBranches: Optimized switch flow. Profile needs to be re-propagated. Data %s consistent.\n",
            fgPgoConsistent ? "is now" : "was already");
        fgPgoConsistent = false;
    }

    Statement*  switchStmt = nullptr;
    LIR::Range* blockRange = nullptr;

    GenTree* switchTree;
    if (block->IsLIR())
    {
        blockRange = &LIR::AsRange(block);
        switchTree = blockRange->LastNode();

        assert(switchTree->OperIs(GT_SWITCH_TABLE));
    }
    else
    {
        switchStmt = block->lastStmt();
        switchTree = switchStmt->GetRootNode();

        assert(switchTree->OperIs(GT_SWITCH));
    }

    noway_assert(switchTree->TypeIs(TYP_VOID));

    // At this point all of the case jump targets have been updated such
    // that none of them go to block that is an empty unconditional block
    // Now check for two trivial switch jumps.
    //
    if (block->GetSwitchTargets()->GetSuccCount() == 1)
    {
        // Use BBJ_ALWAYS for a switch with only a default clause, or with only one unique successor.

        JITDUMP("\nRemoving a switch jump with a single target (" FMT_BB ")\n", block->bbNum);
        JITDUMP("BEFORE:\n");
        DBEXEC(verbose, fgDispBasicBlocks());

        if (block->IsLIR())
        {
            bool               isClosed;
            unsigned           sideEffects;
            LIR::ReadOnlyRange switchTreeRange = blockRange->GetTreeRange(switchTree, &isClosed, &sideEffects);

            // The switch tree should form a contiguous, side-effect free range by construction. See
            // Lowering::LowerSwitch for details.
            assert(isClosed);
            assert((sideEffects & GTF_ALL_EFFECT) == 0);

            blockRange->Delete(this, block, std::move(switchTreeRange));
        }
        else
        {
            /* check for SIDE_EFFECTS */
            if (switchTree->gtFlags & GTF_SIDE_EFFECT)
            {
                /* Extract the side effects from the conditional */
                GenTree* sideEffList = nullptr;

                gtExtractSideEffList(switchTree, &sideEffList);

                if (sideEffList == nullptr)
                {
                    goto NO_SWITCH_SIDE_EFFECT;
                }

                noway_assert(sideEffList->gtFlags & GTF_SIDE_EFFECT);

#ifdef DEBUG
                if (verbose)
                {
                    printf("\nSwitch expression has side effects! Extracting side effects...\n");
                    gtDispTree(switchTree);
                    printf("\n");
                    gtDispTree(sideEffList);
                    printf("\n");
                }
#endif // DEBUG

                /* Replace the conditional statement with the list of side effects */
                noway_assert(!sideEffList->OperIs(GT_SWITCH));

                switchStmt->SetRootNode(sideEffList);

                if (fgNodeThreading != NodeThreading::None)
                {
                    compCurBB = block;

                    /* Update ordering, costs, FP levels, etc. */
                    gtSetStmtInfo(switchStmt);

                    /* Re-link the nodes for this statement */
                    fgSetStmtSeq(switchStmt);
                }
            }
            else
            {

            NO_SWITCH_SIDE_EFFECT:

                /* conditional has NO side effect - remove it */
                fgRemoveStmt(block, switchStmt);
            }
        }

        // Change the switch jump into a BBJ_ALWAYS
        block->SetKindAndTargetEdge(BBJ_ALWAYS, block->GetSwitchTargets()->GetCase(0));
        const unsigned dupCount = block->GetTargetEdge()->getDupCount();
        block->GetTargetEdge()->decrementDupCount(dupCount - 1);
        block->GetTarget()->bbRefs -= (dupCount - 1);
        return true;
    }
    else if (block->GetSwitchTargets()->GetCaseCount() == 2)
    {
        /* Use a BBJ_COND(switchVal==0) for a switch with only one
           significant clause besides the default clause */
        GenTree* switchVal = switchTree->AsOp()->gtOp1;
        noway_assert(genActualTypeIsIntOrI(switchVal->TypeGet()));

        // If we are in LIR, remove the jump table from the block.
        if (block->IsLIR())
        {
            GenTree* jumpTable = switchTree->AsOp()->gtOp2;
            assert(jumpTable->OperIs(GT_JMPTABLE));
            blockRange->Remove(jumpTable);
        }

        // Change the GT_SWITCH(switchVal) into GT_JTRUE(GT_EQ(switchVal==0)).
        // Also mark the node as GTF_DONT_CSE as further down JIT is not capable of handling it.
        // For example CSE could determine that the expression rooted at GT_EQ is a candidate cse and
        // replace it with a COMMA node.  In such a case we will end up with GT_JTRUE node pointing to
        // a COMMA node which results in noway asserts in fgMorphSmpOp(), optAssertionGen() and rpPredictTreeRegUse().
        // For the same reason fgMorphSmpOp() marks GT_JTRUE nodes with RELOP children as GTF_DONT_CSE.

        JITDUMP("\nConverting a switch (" FMT_BB ") with only one significant clause besides a default target to a "
                "conditional branch. Before:\n",
                block->bbNum);
        DISPNODE(switchTree);

        switchTree->ChangeOper(GT_JTRUE);
        GenTree* zeroConstNode    = gtNewZeroConNode(genActualType(switchVal->TypeGet()));
        GenTree* condNode         = gtNewOperNode(GT_EQ, TYP_INT, switchVal, zeroConstNode);
        switchTree->AsOp()->gtOp1 = condNode;
        switchTree->AsOp()->gtOp1->gtFlags |= (GTF_RELOP_JMP_USED | GTF_DONT_CSE);

        if (block->IsLIR())
        {
            blockRange->InsertAfter(switchVal, zeroConstNode, condNode);
            LIR::ReadOnlyRange range(zeroConstNode, switchTree);
            m_pLowering->LowerRange(block, range);
        }
        else if (fgNodeThreading != NodeThreading::None)
        {
            gtSetStmtInfo(switchStmt);
            fgSetStmtSeq(switchStmt);
        }

        FlowEdge* const trueEdge  = block->GetSwitchTargets()->GetCase(0);
        FlowEdge* const falseEdge = block->GetSwitchTargets()->GetCase(1);
        block->SetCond(trueEdge, falseEdge);

        JITDUMP("After:\n");
        DISPNODE(switchTree);

        return true;
    }
    return modified;
}

//-------------------------------------------------------------
// fgBlockEndFavorsTailDuplication:
//     Heuristic function that returns true if this block ends in a statement that looks favorable
//     for tail-duplicating its successor (such as assigning a constant to a local).
//
//  Arguments:
//      block: BasicBlock we are considering duplicating the successor of
//      lclNum: local that is used by the successor block, provided by
//        prior call to fgBlockIsGoodTailDuplicationCandidate
//
//  Returns:
//     true if block end is favorable for tail duplication
//
//  Notes:
//     This is the second half of the evaluation for tail duplication, where we try
//     to determine if this predecessor block assigns a constant or provides useful
//     information about a local that is tested in an unconditionally executed successor.
//     If so then duplicating the successor will likely allow the test to be
//     optimized away.
//
bool Compiler::fgBlockEndFavorsTailDuplication(BasicBlock* block, unsigned lclNum)
{
    if (block->isRunRarely())
    {
        return false;
    }

    // If the local is address exposed, we currently can't optimize.
    //
    LclVarDsc* const lclDsc = lvaGetDesc(lclNum);

    if (lclDsc->IsAddressExposed())
    {
        return false;
    }

    Statement* const lastStmt  = block->lastStmt();
    Statement* const firstStmt = block->FirstNonPhiDef();

    if (lastStmt == nullptr)
    {
        return false;
    }

    // Tail duplication tends to pay off when the last statement
    // is a local store of a constant, arraylength, or a relop.
    // This is because these statements produce information about values
    // that would otherwise be lost at the upcoming merge point.
    //
    // Check up to N statements...
    //
    const int  limit = 2;
    int        count = 0;
    Statement* stmt  = lastStmt;

    while (count < limit)
    {
        count++;
        GenTree* const tree = stmt->GetRootNode();
        if (tree->OperIsLocalStore() && !tree->OperIsBlkOp() && (tree->AsLclVarCommon()->GetLclNum() == lclNum))
        {
            GenTree* const value = tree->Data();
            if (value->OperIsArrLength() || value->OperIsConst() || value->OperIsCompare())
            {
                return true;
            }
        }

        Statement* const prevStmt = stmt->GetPrevStmt();

        // The statement list prev links wrap from first->last, so exit
        // when we see lastStmt again, as we've now seen all statements.
        //
        if (prevStmt == lastStmt)
        {
            break;
        }

        stmt = prevStmt;
    }

    return false;
}

//-------------------------------------------------------------
// fgBlockIsGoodTailDuplicationCandidate:
//     Heuristic function that examines a block (presumably one that is a merge point) to determine
//     if it is a good candidate to be duplicated.
//
// Arguments:
//     target - the tail block (candidate for duplication)
//
// Returns:
//     true if this is a good candidate, false otherwise
//     if true, lclNum is set to lcl to scan for in predecessor block
//
// Notes:
//     The current heuristic is that tail duplication is deemed favorable if this
//     block simply tests the value of a local against a constant or some other local.
//
//     This is the first half of the evaluation for tail duplication. We subsequently
//     need to check if predecessors of this block assigns a constant to the local.
//
bool Compiler::fgBlockIsGoodTailDuplicationCandidate(BasicBlock* target, unsigned* lclNum)
{
    *lclNum = BAD_VAR_NUM;

    // Here we are looking for small blocks where a local live-into the block
    // ultimately feeds a simple conditional branch.
    //
    // These blocks are small, and when duplicated onto the tail of blocks that end in
    // local stores, there is a high probability of the branch completely going away.
    //
    // This is by no means the only kind of tail that it is beneficial to duplicate,
    // just the only one we recognize for now.
    if (!target->KindIs(BBJ_COND))
    {
        return false;
    }

    // No point duplicating this block if it's not a control flow join.
    if (target->bbRefs < 2)
    {
        return false;
    }

    // No point duplicating this block if it would not remove (part of) the join.
    if (target->TrueTargetIs(target) || target->FalseTargetIs(target))
    {
        return false;
    }

    Statement* const lastStmt  = target->lastStmt();
    Statement* const firstStmt = target->FirstNonPhiDef();

    // We currently allow just one statement aside from the branch.
    //
    if ((firstStmt != lastStmt) && (firstStmt != lastStmt->GetPrevStmt()))
    {
        return false;
    }

    // Verify the branch is just a simple local compare.
    //
    GenTree* const lastTree = lastStmt->GetRootNode();

    if (!lastTree->OperIs(GT_JTRUE))
    {
        return false;
    }

    // must be some kind of relational operator
    GenTree* const cond = lastTree->AsOp()->gtOp1;
    if (!cond->OperIsCompare())
    {
        return false;
    }

    // op1 must be some combinations of casts of local or constant
    GenTree* op1 = cond->AsOp()->gtOp1;
    while (op1->OperIs(GT_CAST))
    {
        op1 = op1->AsOp()->gtOp1;
    }

    if (!op1->IsLocal() && !op1->OperIsConst())
    {
        return false;
    }

    // op2 must be some combinations of casts of local or constant
    GenTree* op2 = cond->AsOp()->gtOp2;
    while (op2->OperIs(GT_CAST))
    {
        op2 = op2->AsOp()->gtOp1;
    }

    if (!op2->IsLocal() && !op2->OperIsConst())
    {
        return false;
    }

    // Tree must have one constant and one local, or be comparing
    // the same local to itself.
    unsigned lcl1 = BAD_VAR_NUM;
    unsigned lcl2 = BAD_VAR_NUM;

    if (op1->IsLocal())
    {
        lcl1 = op1->AsLclVarCommon()->GetLclNum();
    }

    if (op2->IsLocal())
    {
        lcl2 = op2->AsLclVarCommon()->GetLclNum();
    }

    if ((lcl1 != BAD_VAR_NUM) && op2->OperIsConst())
    {
        *lclNum = lcl1;
    }
    else if ((lcl2 != BAD_VAR_NUM) && op1->OperIsConst())
    {
        *lclNum = lcl2;
    }
    else if ((lcl1 != BAD_VAR_NUM) && (lcl1 == lcl2))
    {
        *lclNum = lcl1;
    }
    else
    {
        return false;
    }

    // If there's no second statement, we're good.
    //
    if (firstStmt == lastStmt)
    {
        return true;
    }

    // Otherwise check the first stmt.
    // Verify the branch is just a simple local compare.
    //
    GenTree* const firstTree = firstStmt->GetRootNode();
    if (!firstTree->OperIs(GT_STORE_LCL_VAR))
    {
        return false;
    }

    unsigned storeLclNum = firstTree->AsLclVar()->GetLclNum();

    if (storeLclNum != *lclNum)
    {
        return false;
    }

    // Could allow unary here too...
    //
    GenTree* const data = firstTree->AsLclVar()->Data();
    if (!data->OperIsBinary())
    {
        return false;
    }

    // op1 must be some combinations of casts of local or constant
    // (or unary)
    op1 = data->AsOp()->gtOp1;
    while (op1->OperIs(GT_CAST))
    {
        op1 = op1->AsOp()->gtOp1;
    }

    if (!op1->IsLocal() && !op1->OperIsConst())
    {
        return false;
    }

    // op2 must be some combinations of casts of local or constant
    // (or unary)
    op2 = data->AsOp()->gtOp2;

    // A binop may not actually have an op2.
    //
    if (op2 == nullptr)
    {
        return false;
    }

    while (op2->OperIs(GT_CAST))
    {
        op2 = op2->AsOp()->gtOp1;
    }

    if (!op2->IsLocal() && !op2->OperIsConst())
    {
        return false;
    }

    // Tree must have one constant and one local, or be comparing
    // the same local to itself.
    lcl1 = BAD_VAR_NUM;
    lcl2 = BAD_VAR_NUM;

    if (op1->IsLocal())
    {
        lcl1 = op1->AsLclVarCommon()->GetLclNum();
    }

    if (op2->IsLocal())
    {
        lcl2 = op2->AsLclVarCommon()->GetLclNum();
    }

    if ((lcl1 != BAD_VAR_NUM) && op2->OperIsConst())
    {
        *lclNum = lcl1;
    }
    else if ((lcl2 != BAD_VAR_NUM) && op1->OperIsConst())
    {
        *lclNum = lcl2;
    }
    else if ((lcl1 != BAD_VAR_NUM) && (lcl1 == lcl2))
    {
        *lclNum = lcl1;
    }
    else
    {
        return false;
    }

    return true;
}

//-------------------------------------------------------------
// fgOptimizeUncondBranchToSimpleCond:
//    For a block which has an unconditional branch, look to see if its target block
//    is a good candidate for tail duplication, and if so do that duplication.
//
// Arguments:
//    block  - block with uncond branch
//    target - block which is target of first block
//
// Returns: true if changes were made
//
// Notes:
//   This optimization generally reduces code size and path length.
//
bool Compiler::fgOptimizeUncondBranchToSimpleCond(BasicBlock* block, BasicBlock* target)
{
    JITDUMP("Considering uncond to cond " FMT_BB " -> " FMT_BB "\n", block->bbNum, target->bbNum);

    if (!BasicBlock::sameEHRegion(block, target))
    {
        return false;
    }

    unsigned lclNum = BAD_VAR_NUM;

    // First check if the successor tests a local and then branches on the result
    // of a test, and obtain the local if so.
    //
    if (!fgBlockIsGoodTailDuplicationCandidate(target, &lclNum))
    {
        return false;
    }

    // At this point we know target is BBJ_COND.
    assert(target->KindIs(BBJ_COND));

    // See if this block assigns constant or other interesting tree to that same local.
    //
    if (!fgBlockEndFavorsTailDuplication(block, lclNum))
    {
        return false;
    }

    // NOTE: we do not currently hit this assert because this function is only called when
    // `fgUpdateFlowGraph` has been called with `doTailDuplication` set to true, and the
    // backend always calls `fgUpdateFlowGraph` with `doTailDuplication` set to false.
    assert(!block->IsLIR());

    // Duplicate the target block at the end of this block
    //
    for (Statement* stmt : target->NonPhiStatements())
    {
        GenTree* clone = gtCloneExpr(stmt->GetRootNode());
        noway_assert(clone);
        Statement* cloneStmt = gtNewStmt(clone);

        if (fgNodeThreading != NodeThreading::None)
        {
            gtSetStmtInfo(cloneStmt);
        }

        fgInsertStmtAtEnd(block, cloneStmt);
    }

    // Fix up block's flow.
    // Assume edge likelihoods transfer over.
    //
    fgRedirectEdge(block->TargetEdgeRef(), target->GetTrueTarget());
    block->GetTargetEdge()->setLikelihood(target->GetTrueEdge()->getLikelihood());
    block->GetTargetEdge()->setHeuristicBased(target->GetTrueEdge()->isHeuristicBased());

    FlowEdge* const falseEdge = fgAddRefPred(target->GetFalseTarget(), block, target->GetFalseEdge());
    block->SetCond(block->GetTargetEdge(), falseEdge);

    JITDUMP("fgOptimizeUncondBranchToSimpleCond(from " FMT_BB " to cond " FMT_BB "), modified " FMT_BB "\n",
            block->bbNum, target->bbNum, block->bbNum);
    JITDUMP("   expecting opts to key off V%02u in " FMT_BB "\n", lclNum, block->bbNum);

    if (target->hasProfileWeight() && block->hasProfileWeight())
    {
        // Remove weight from target since block now bypasses it...
        //
        weight_t targetWeight = target->bbWeight;
        weight_t blockWeight  = block->bbWeight;
        target->decreaseBBProfileWeight(blockWeight);
        JITDUMP("Decreased " FMT_BB " profile weight from " FMT_WT " to " FMT_WT "\n", target->bbNum, targetWeight,
                target->bbWeight);
    }

    return true;
}

//-------------------------------------------------------------
// fgFoldSimpleCondByForwardSub:
//   Try to refine the flow of a block that may have just been tail duplicated
//   or compacted.
//
// Arguments:
//   block - block that was tail duplicated or compacted
//
// Returns Value:
//   true if control flow was changed
//
bool Compiler::fgFoldSimpleCondByForwardSub(BasicBlock* block)
{
    assert(block->KindIs(BBJ_COND));
    GenTree* jtrue = block->lastStmt()->GetRootNode();
    assert(jtrue->OperIs(GT_JTRUE));

    GenTree* relop = jtrue->gtGetOp1();
    if (!relop->OperIsCompare())
    {
        return false;
    }

    GenTree* op1 = relop->gtGetOp1();
    GenTree* op2 = relop->gtGetOp2();

    GenTree**            lclUse;
    GenTreeLclVarCommon* lcl;

    if (op1->OperIs(GT_LCL_VAR) && op2->IsIntegralConst())
    {
        lclUse = &relop->AsOp()->gtOp1;
        lcl    = op1->AsLclVarCommon();
    }
    else if (op2->OperIs(GT_LCL_VAR) && op1->IsIntegralConst())
    {
        lclUse = &relop->AsOp()->gtOp2;
        lcl    = op2->AsLclVarCommon();
    }
    else
    {
        return false;
    }

    Statement* secondLastStmt = block->lastStmt()->GetPrevStmt();
    if ((secondLastStmt == nullptr) || (secondLastStmt == block->lastStmt()))
    {
        return false;
    }

    GenTree* prevTree = secondLastStmt->GetRootNode();
    if (!prevTree->OperIs(GT_STORE_LCL_VAR))
    {
        return false;
    }

    GenTreeLclVarCommon* store = prevTree->AsLclVarCommon();
    if (store->GetLclNum() != lcl->GetLclNum())
    {
        return false;
    }

    if (!store->Data()->IsIntegralConst())
    {
        return false;
    }

    if (genActualType(store) != genActualType(store->Data()) || (genActualType(store) != genActualType(lcl)))
    {
        return false;
    }

    JITDUMP("Forward substituting local after jump threading. Before:\n");
    DISPSTMT(block->lastStmt());

    JITDUMP("\nAfter:\n");

    LclVarDsc* varDsc  = lvaGetDesc(lcl);
    GenTree*   newData = gtCloneExpr(store->Data());
    if (varTypeIsSmall(varDsc) && fgCastNeeded(store->Data(), varDsc->TypeGet()))
    {
        newData = gtNewCastNode(TYP_INT, newData, false, varDsc->TypeGet());
        newData = gtFoldExpr(newData);
    }

    *lclUse = newData;
    DISPSTMT(block->lastStmt());

    JITDUMP("\nNow trying to fold...\n");
    jtrue->AsUnOp()->gtOp1 = gtFoldExpr(relop);
    DISPSTMT(block->lastStmt());

    Compiler::FoldResult result = fgFoldConditional(block);
    if (result != Compiler::FoldResult::FOLD_DID_NOTHING)
    {
        assert(block->KindIs(BBJ_ALWAYS));
        return true;
    }

    return false;
}

//-------------------------------------------------------------
// fgRemoveConditionalJump:
//    Optimize a BBJ_COND block that unconditionally jumps to the same target
//
// Arguments:
//    block - BBJ_COND block with identical true/false targets
//
void Compiler::fgRemoveConditionalJump(BasicBlock* block)
{
    assert(block->KindIs(BBJ_COND));
    assert(block->TrueEdgeIs(block->GetFalseEdge()));

    BasicBlock* target = block->GetTrueTarget();

#ifdef DEBUG
    if (verbose)
    {
        printf("Block " FMT_BB " becoming a BBJ_ALWAYS to " FMT_BB " (jump target is the same whether the condition"
               " is true or false)\n",
               block->bbNum, target->bbNum);
    }
#endif // DEBUG

    if (block->IsLIR())
    {
        LIR::Range& blockRange = LIR::AsRange(block);
        GenTree*    jmp        = blockRange.LastNode();
        assert(jmp->OperIsConditionalJump());

        bool               isClosed;
        unsigned           sideEffects;
        LIR::ReadOnlyRange jmpRange;

        if (jmp->OperIs(GT_JCC))
        {
            // For JCC we have an invariant until resolution that the
            // previous node sets those CPU flags.
            GenTree* prevNode = jmp->gtPrev;
            assert((prevNode != nullptr) && ((prevNode->gtFlags & GTF_SET_FLAGS) != 0));
            prevNode->gtFlags &= ~GTF_SET_FLAGS;
            jmpRange = blockRange.GetTreeRange(prevNode, &isClosed, &sideEffects);
            jmpRange = LIR::ReadOnlyRange(jmpRange.FirstNode(), jmp);
        }
        else
        {
            jmpRange = blockRange.GetTreeRange(jmp, &isClosed, &sideEffects);
        }

        if (isClosed && ((sideEffects & GTF_SIDE_EFFECT) == 0))
        {
            // If the jump and its operands form a contiguous, side-effect-free range,
            // remove them.
            blockRange.Delete(this, block, std::move(jmpRange));
        }
        else
        {
            // Otherwise, just remove the jump node itself.
            blockRange.Remove(jmp, true);
        }
    }
    else
    {
        Statement* condStmt = block->lastStmt();
        GenTree*   cond     = condStmt->GetRootNode();
        noway_assert(cond->OperIs(GT_JTRUE));

        /* check for SIDE_EFFECTS */
        if (cond->gtFlags & GTF_SIDE_EFFECT)
        {
            /* Extract the side effects from the conditional */
            GenTree* sideEffList = nullptr;

            gtExtractSideEffList(cond, &sideEffList);

            if (sideEffList == nullptr)
            {
                compCurBB = block;
                fgRemoveStmt(block, condStmt);
            }
            else
            {
                noway_assert(sideEffList->gtFlags & GTF_SIDE_EFFECT);
#ifdef DEBUG
                if (verbose)
                {
                    printf("\nConditional has side effects! Extracting side effects...\n");
                    gtDispTree(cond);
                    printf("\n");
                    gtDispTree(sideEffList);
                    printf("\n");
                }
#endif // DEBUG

                /* Replace the conditional statement with the list of side effects */
                noway_assert(!sideEffList->OperIs(GT_JTRUE));

                condStmt->SetRootNode(sideEffList);

                if (fgNodeThreading == NodeThreading::AllTrees)
                {
                    compCurBB = block;

                    /* Update ordering, costs, FP levels, etc. */
                    gtSetStmtInfo(condStmt);

                    /* Re-link the nodes for this statement */
                    fgSetStmtSeq(condStmt);
                }
            }
        }
        else
        {
            compCurBB = block;
            /* conditional has NO side effect - remove it */
            fgRemoveStmt(block, condStmt);
        }
    }

    /* Conditional is gone - always jump to target */

    block->SetKindAndTargetEdge(BBJ_ALWAYS, block->GetTrueEdge());
    assert(block->TargetIs(target));

    /* Update bbRefs and bbNum - Conditional predecessors to the same
     * block are counted twice so we have to remove one of them */

    noway_assert(target->countOfInEdges() > 1);
    fgRemoveRefPred(block->GetTargetEdge());
}

//-------------------------------------------------------------
// fgOptimizeBranch: Optimize an unconditional branch that branches to a conditional branch.
//
// Currently we require that the conditional branch jump back to the block that follows the unconditional
// branch. We can improve the code execution and layout by concatenating a copy of the conditional branch
// block at the end of the conditional branch and reversing the sense of the branch.
//
// This is only done when the amount of code to be copied is smaller than our calculated threshold
// in maxDupCostSz.
//
// Arguments:
//    bJump - block with branch
//
// Returns: true if changes were made
//
bool Compiler::fgOptimizeBranch(BasicBlock* bJump)
{
    assert(opts.OptimizationEnabled());

    if (!bJump->KindIs(BBJ_ALWAYS))
    {
        return false;
    }

    if (bJump->JumpsToNext())
    {
        return false;
    }

    if (bJump->HasFlag(BBF_KEEP_BBJ_ALWAYS))
    {
        return false;
    }

    BasicBlock* const bDest = bJump->GetTarget();

    if (!bDest->KindIs(BBJ_COND))
    {
        return false;
    }

    if (!bJump->NextIs(bDest->GetTrueTarget()))
    {
        return false;
    }

    // 'bJump' must be in the same try region as the condition, since we're going to insert
    // a duplicated condition in 'bJump', and the condition might include exception throwing code.
    if (!BasicBlock::sameTryRegion(bJump, bDest))
    {
        return false;
    }

    // We should have already compacted 'bDest' into 'bJump', if it is possible.
    assert(!fgCanCompactBlock(bJump));

    BasicBlock* const trueTarget  = bDest->GetTrueTarget();
    BasicBlock* const falseTarget = bDest->GetFalseTarget();

    // This function is only called in the frontend.
    assert(!bJump->IsLIR());
    assert(!bDest->IsLIR());

    unsigned estDupCostSz = 0;
    for (Statement* const stmt : bDest->Statements())
    {
        // We want to compute the costs of the statement. Unfortunately, gtPrepareCost() / gtSetStmtInfo()
        // call gtSetEvalOrder(), which can reorder nodes. If it does so, we need to re-thread the gtNext/gtPrev
        // links. We don't know if it does or doesn't reorder nodes, so we end up always re-threading the links.

        gtSetStmtInfo(stmt);
        if (fgNodeThreading == NodeThreading::AllTrees)
        {
            fgSetStmtSeq(stmt);
        }

        GenTree* expr = stmt->GetRootNode();
        estDupCostSz += expr->GetCostSz();
    }

    bool     haveProfileWeights = false;
    weight_t weightJump         = bJump->bbWeight;
    weight_t weightDest         = bDest->bbWeight;
    weight_t weightNext         = trueTarget->bbWeight;
    bool     rareJump           = bJump->isRunRarely();
    bool     rareDest           = bDest->isRunRarely();
    bool     rareNext           = trueTarget->isRunRarely();

    // If we have profile data then we calculate the number of time
    // the loop will iterate into loopIterations
    if (fgIsUsingProfileWeights())
    {
        // Only rely upon the profile weight when all three of these blocks
        // have either good profile weights or are rarely run
        //
        if ((bJump->hasProfileWeight() || bJump->isRunRarely()) &&
            (bDest->hasProfileWeight() || bDest->isRunRarely()) &&
            (trueTarget->hasProfileWeight() || trueTarget->isRunRarely()))
        {
            haveProfileWeights = true;

            if ((weightJump * 100) < weightDest)
            {
                rareJump = true;
            }

            if ((weightNext * 100) < weightDest)
            {
                rareNext = true;
            }

            if (((weightDest * 100) < weightJump) && ((weightDest * 100) < weightNext))
            {
                rareDest = true;
            }
        }
    }

    unsigned maxDupCostSz = 6;

    //
    // Branches between the hot and rarely run regions
    // should be minimized.  So we allow a larger size
    //
    if (rareDest != rareJump)
    {
        maxDupCostSz += 6;
    }

    if (rareDest != rareNext)
    {
        maxDupCostSz += 6;
    }

    //
    // If we are AOT compiling: if the unconditional branch is a rarely run block then we are willing to have
    // more code expansion since we won't be running code from this page.
    //
    if (IsAot())
    {
        if (rareJump)
        {
            maxDupCostSz *= 2;
        }
    }

    // If the compare has too high cost then we don't want to dup

    bool costIsTooHigh = (estDupCostSz > maxDupCostSz);

#ifdef DEBUG
    if (verbose)
    {
        printf("\nDuplication of the conditional block " FMT_BB " (always branch from " FMT_BB
               ") %s, because the cost of duplication (%i) is %s than %i, haveProfileWeights = %s\n",
               bDest->bbNum, bJump->bbNum, costIsTooHigh ? "not done" : "performed", estDupCostSz,
               costIsTooHigh ? "greater" : "less or equal", maxDupCostSz, dspBool(haveProfileWeights));
    }
#endif // DEBUG

    // Computing the duplication cost may have triggered node reordering, so return true to indicate we modified IR
    if (costIsTooHigh)
    {
        return true;
    }

    /* Looks good - duplicate the conditional block */

    Statement* newStmtList = nullptr; // new stmt list to be added to bJump
    Statement* newLastStmt = nullptr;

    /* Visit all the statements in bDest */

    for (Statement* const curStmt : bDest->NonPhiStatements())
    {
        // Clone/substitute the expression.
        Statement* stmt = gtCloneStmt(curStmt);
        assert(stmt != nullptr);

        if (fgNodeThreading == NodeThreading::AllTrees)
        {
            gtSetStmtInfo(stmt);
            fgSetStmtSeq(stmt);
        }

        /* Append the expression to our list */

        if (newStmtList != nullptr)
        {
            newLastStmt->SetNextStmt(stmt);
        }
        else
        {
            newStmtList = stmt;
        }

        stmt->SetPrevStmt(newLastStmt);
        newLastStmt = stmt;
    }

    // Get to the condition node from the statement tree.
    GenTree* condTree = newLastStmt->GetRootNode();
    noway_assert(condTree->OperIs(GT_JTRUE));

    // Set condTree to the operand to the GT_JTRUE.
    condTree = condTree->gtGetOp1();

    // This condTree has to be a RelOp comparison.
    // If not, return true since we created new nodes.
    if (!condTree->OperIsCompare())
    {
        return true;
    }

    // Join the two linked lists.
    Statement* lastStmt = bJump->lastStmt();

    if (lastStmt != nullptr)
    {
        Statement* stmt = bJump->firstStmt();
        stmt->SetPrevStmt(newLastStmt);
        lastStmt->SetNextStmt(newStmtList);
        newStmtList->SetPrevStmt(lastStmt);
    }
    else
    {
        bJump->bbStmtList = newStmtList;
        newStmtList->SetPrevStmt(newLastStmt);
    }

    // We need to update the following flags of the bJump block if they were set in the bDest block
    bJump->CopyFlags(bDest, BBF_COPY_PROPAGATE);

    // Update bbRefs and bbPreds
    //
    FlowEdge* const falseEdge = bDest->GetFalseEdge();
    FlowEdge* const trueEdge  = bDest->GetTrueEdge();

    fgRedirectEdge(bJump->TargetEdgeRef(), falseTarget);
    bJump->GetTargetEdge()->setLikelihood(falseEdge->getLikelihood());
    bJump->GetTargetEdge()->setHeuristicBased(falseEdge->isHeuristicBased());

    FlowEdge* const newTrueEdge = fgAddRefPred(trueTarget, bJump, trueEdge);

    bJump->SetCond(newTrueEdge, bJump->GetTargetEdge());

    // Update profile data
    //
    if (haveProfileWeights)
    {
        // bJump no longer flows into bDest
        //
        bDest->decreaseBBProfileWeight(bJump->bbWeight);

        // Propagate bJump's weight into its new successors
        //
        trueTarget->setBBProfileWeight(trueTarget->computeIncomingWeight());
        falseTarget->setBBProfileWeight(falseTarget->computeIncomingWeight());

        if ((trueTarget->NumSucc() > 0) || (falseTarget->NumSucc() > 0))
        {
            JITDUMP("fgOptimizeBranch: New flow out of " FMT_BB " needs to be propagated. Data %s inconsistent.\n",
                    bJump->bbNum, fgPgoConsistent ? "is now" : "was already");
            fgPgoConsistent = false;
        }
    }

#if DEBUG
    if (verbose)
    {
        // Dump out the newStmtList that we created
        printf("\nfgOptimizeBranch added these statements(s) at the end of " FMT_BB ":\n", bJump->bbNum);
        for (Statement* stmt : StatementList(newStmtList))
        {
            gtDispStmt(stmt);
        }
        printf("\nfgOptimizeBranch changed block " FMT_BB " from BBJ_ALWAYS to BBJ_COND.\n", bJump->bbNum);

        printf("\nAfter this change in fgOptimizeBranch the BB graph is:");
        fgDispBasicBlocks(verboseTrees);
        printf("\n");
    }
#endif // DEBUG

    // Removing flow from 'bJump' into 'bDest' may have made it possible to compact the latter.
    BasicBlock* const uniquePred = bDest->GetUniquePred(this);
    if ((uniquePred != nullptr) && fgCanCompactBlock(uniquePred))
    {
        JITDUMP(FMT_BB " can now be compacted into its remaining predecessor.\n", bDest->bbNum);
        fgCompactBlock(uniquePred);
    }

    return true;
}

//-----------------------------------------------------------------------------
// fgPeelSwitch: Modify a switch to check for its dominant case up front.
//
// Parameters:
//   block - The switch block with a dominant case
//
void Compiler::fgPeelSwitch(BasicBlock* block)
{
    assert(block->KindIs(BBJ_SWITCH));
    assert(block->GetSwitchTargets()->HasDominantCase());
    assert(!block->isRunRarely());

    // Lowering expands switches, so calling this method on lowered IR
    // does not make sense.
    //
    assert(!block->IsLIR());

    // We currently will only see dominant cases with PGO.
    //
    assert(block->hasProfileWeight());

    const unsigned dominantCase = block->GetSwitchTargets()->GetDominantCase();
    JITDUMP(FMT_BB " has switch with dominant case %u, considering peeling\n", block->bbNum, dominantCase);

    // The dominant case should not be the default case, as we already peel that one.
    //
    assert(dominantCase < (block->GetSwitchTargets()->GetCaseCount() - 1));
    FlowEdge* const   dominantEdge   = block->GetSwitchTargets()->GetCase(dominantCase);
    BasicBlock* const dominantTarget = dominantEdge->getDestinationBlock();
    Statement* const  switchStmt     = block->lastStmt();
    GenTree* const    switchTree     = switchStmt->GetRootNode();
    assert(switchTree->OperIs(GT_SWITCH));
    GenTree* const switchValue = switchTree->gtGetOp1();

    // Split the switch block just before at the switch.
    //
    // After this, newBlock is the switch block, and
    // block is the upstream block.
    //
    BasicBlock* newBlock = nullptr;

    if (block->firstStmt() == switchStmt)
    {
        newBlock = fgSplitBlockAtBeginning(block);
    }
    else
    {
        newBlock = fgSplitBlockAfterStatement(block, switchStmt->GetPrevStmt());
    }

    // Set up a compare in the upstream block, "stealing" the switch value tree.
    //
    GenTree* const   dominantCaseCompare = gtNewOperNode(GT_EQ, TYP_INT, switchValue, gtNewIconNode(dominantCase));
    GenTree* const   jmpTree             = gtNewOperNode(GT_JTRUE, TYP_VOID, dominantCaseCompare);
    Statement* const jmpStmt             = fgNewStmtFromTree(jmpTree, switchStmt->GetDebugInfo());
    fgInsertStmtAtEnd(block, jmpStmt);

    // Reattach switch value to the switch. This may introduce a comma
    // in the upstream compare tree, if the switch value expression is complex.
    //
    switchTree->AsOp()->gtOp1 = fgMakeMultiUse(&dominantCaseCompare->AsOp()->gtOp1);

    // Update flags
    //
    switchTree->gtFlags = switchTree->gtGetOp1()->gtFlags & GTF_ALL_EFFECT;
    dominantCaseCompare->gtFlags |= dominantCaseCompare->gtGetOp1()->gtFlags & GTF_ALL_EFFECT;
    jmpTree->gtFlags |= dominantCaseCompare->gtFlags & GTF_ALL_EFFECT;
    dominantCaseCompare->gtFlags |= GTF_RELOP_JMP_USED | GTF_DONT_CSE;

    // Wire up the new control flow.
    //
    FlowEdge* const blockToTargetEdge   = fgAddRefPred(dominantTarget, block, dominantEdge);
    FlowEdge* const blockToNewBlockEdge = newBlock->bbPreds;
    block->SetCond(blockToTargetEdge, blockToNewBlockEdge);

    // Update profile data
    //
    const weight_t fraction            = dominantEdge->getLikelihood();
    const weight_t blockToTargetWeight = block->bbWeight * fraction;

    newBlock->decreaseBBProfileWeight(blockToTargetWeight);
    blockToNewBlockEdge->setLikelihood(max(0.0, 1.0 - fraction));

    // Now that the dominant case has been peeled, set the case's edge likelihood to zero,
    // and increase all other case likelihoods proportionally.
    //
    dominantEdge->setLikelihood(BB_ZERO_WEIGHT);
    const unsigned   numSucc = newBlock->GetSwitchTargets()->GetSuccCount();
    FlowEdge** const jumpTab = newBlock->GetSwitchTargets()->GetSuccs();
    for (unsigned i = 0; i < numSucc; i++)
    {
        // If we removed all of the flow out of 'block', distribute flow among the remaining edges evenly.
        const weight_t currLikelihood = jumpTab[i]->getLikelihood();
        const weight_t newLikelihood  = (fraction == 1.0) ? (1.0 / numSucc) : (currLikelihood / (1.0 - fraction));
        jumpTab[i]->setLikelihood(min(1.0, newLikelihood));
    }

    // For now we leave the switch as is, since there's no way
    // to indicate that one of the cases is now unreachable.
    //
    // But it no longer has a dominant case.
    //
    newBlock->GetSwitchTargets()->RemoveDominantCase();

    if (fgNodeThreading == NodeThreading::AllTrees)
    {
        // The switch tree has been modified.
        JITDUMP("Rethreading " FMT_STMT "\n", switchStmt->GetID());
        gtSetStmtInfo(switchStmt);
        fgSetStmtSeq(switchStmt);

        // fgNewStmtFromTree() already threaded the tree, but calling fgMakeMultiUse() might have
        // added new nodes if a COMMA was introduced.
        JITDUMP("Rethreading " FMT_STMT "\n", jmpStmt->GetID());
        gtSetStmtInfo(jmpStmt);
        fgSetStmtSeq(jmpStmt);
    }
}

//-----------------------------------------------------------------------------
// fgExpandRunRarelyBlocks: given the current set of run rarely blocks,
//   see if we can deduce that some other blocks are run rarely.
//
// Returns:
//    True if new block was marked as run rarely.
//
bool Compiler::fgExpandRarelyRunBlocks()
{
    bool result = false;

#ifdef DEBUG
    if (verbose)
    {
        printf("\n*************** In fgExpandRarelyRunBlocks()\n");
    }

    const char* reason = nullptr;
#endif

    // Helper routine to figure out the lexically earliest predecessor
    // of bPrev that could become run rarely, given that bPrev
    // has just become run rarely.
    //
    // Note this is potentially expensive for large flow graphs and blocks
    // with lots of predecessors.
    //
    auto newRunRarely = [](BasicBlock* block, BasicBlock* bPrev) {
        // Figure out earliest block that might be impacted
        BasicBlock* bPrevPrev = nullptr;
        BasicBlock* tmpbb;

        if (bPrev->KindIs(BBJ_CALLFINALLYRET))
        {
            // If we've got a BBJ_CALLFINALLY/BBJ_CALLFINALLYRET pair, treat the BBJ_CALLFINALLY as an
            // additional predecessor for the BBJ_CALLFINALLYRET block
            tmpbb = bPrev->Prev();
            noway_assert(tmpbb->isBBCallFinallyPair());
            bPrevPrev = tmpbb;
        }

        FlowEdge* pred = bPrev->bbPreds;

        if (pred != nullptr)
        {
            // bPrevPrev will be set to the lexically
            // earliest predecessor of bPrev.

            while (pred != nullptr)
            {
                if (bPrevPrev == nullptr)
                {
                    // Initially we select the first block in the bbPreds list
                    bPrevPrev = pred->getSourceBlock();
                    continue;
                }

                // Walk the flow graph lexically forward from pred->getBlock()
                // if we find (block == bPrevPrev) then
                // pred->getBlock() is an earlier predecessor.
                for (tmpbb = pred->getSourceBlock(); tmpbb != nullptr; tmpbb = tmpbb->Next())
                {
                    if (tmpbb == bPrevPrev)
                    {
                        /* We found an earlier predecessor */
                        bPrevPrev = pred->getSourceBlock();
                        break;
                    }
                    else if (tmpbb == bPrev)
                    {
                        // We have reached bPrev so stop walking
                        // as this cannot be an earlier predecessor
                        break;
                    }
                }

                // Onto the next predecessor
                pred = pred->getNextPredEdge();
            }
        }

        if (bPrevPrev != nullptr)
        {
            // Walk the flow graph forward from bPrevPrev
            // if we don't find (tmpbb == bPrev) then our candidate
            // bPrevPrev is lexically after bPrev and we do not
            // want to select it as our new block

            for (tmpbb = bPrevPrev; tmpbb != nullptr; tmpbb = tmpbb->Next())
            {
                if (tmpbb == bPrev)
                {
                    // Set up block back to the lexically
                    // earliest predecessor of pPrev

                    return bPrevPrev;
                }
            }
        }

        // No reason to backtrack
        //
        return (BasicBlock*)nullptr;
    };

    // We expand the number of rarely run blocks by observing
    // that a block that falls into or jumps to a rarely run block,
    // must itself be rarely run and when we have a conditional
    // jump in which both branches go to rarely run blocks then
    // the block must itself be rarely run

    BasicBlock* block;
    BasicBlock* bPrev;

    for (bPrev = fgFirstBB, block = bPrev->Next(); block != nullptr; bPrev = block, block = block->Next())
    {
        if (bPrev->isRunRarely())
        {
            continue;
        }

        if (bPrev->hasProfileWeight())
        {
            continue;
        }

        INDEBUG(const char* reason = nullptr);
        bool setRarelyRun = false;

        switch (bPrev->GetKind())
        {
            case BBJ_ALWAYS:
                if (bPrev->GetTarget()->isRunRarely())
                {
                    INDEBUG(reason = "Unconditional jump to a rarely run block");
                    setRarelyRun = true;
                }
                break;

            case BBJ_CALLFINALLY:
                if (bPrev->isBBCallFinallyPair() && block->isRunRarely())
                {
                    INDEBUG(reason = "Call of finally followed rarely run continuation block");
                    setRarelyRun = true;
                }
                break;

            case BBJ_CALLFINALLYRET:
                if (bPrev->GetFinallyContinuation()->isRunRarely())
                {
                    INDEBUG(reason = "Finally continuation is a rarely run block");
                    setRarelyRun = true;
                }
                break;

            case BBJ_COND:
                if (bPrev->GetTrueTarget()->isRunRarely() && bPrev->GetFalseTarget()->isRunRarely())
                {
                    INDEBUG(reason = "Both sides of a conditional jump are rarely run");
                    setRarelyRun = true;
                }
                break;

            default:
                break;
        }

        if (setRarelyRun)
        {
            JITDUMP("%s, marking " FMT_BB " as rarely run\n", reason, bPrev->bbNum);

            // Must not have previously been marked
            noway_assert(!bPrev->isRunRarely());

            // Mark bPrev as a new rarely run block
            bPrev->bbSetRunRarely();

            // We have marked at least one block.
            //
            result = true;

            // See if we should to backtrack.
            //
            BasicBlock* bContinue = newRunRarely(block, bPrev);

            // If so, reset block to the backtrack point.
            //
            if (bContinue != nullptr)
            {
                block = bContinue;
            }
        }
    }

    // Now iterate over every block to see if we can prove that a block is rarely run
    // (i.e. when all predecessors to the block are rarely run)
    //
    for (bPrev = fgFirstBB, block = bPrev->Next(); block != nullptr; bPrev = block, block = block->Next())
    {
        // If block is not run rarely, then check to make sure that it has
        // at least one non-rarely run block.

        if (!block->isRunRarely() && !block->isBBCallFinallyPairTail())
        {
            bool rare = true;

            /* Make sure that block has at least one normal predecessor */
            for (BasicBlock* const predBlock : block->PredBlocks())
            {
                /* Find the fall through predecessor, if any */
                if (!predBlock->isRunRarely())
                {
                    rare = false;
                    break;
                }
            }

            if (rare)
            {
                // If 'block' is the start of a handler or filter then we cannot make it
                // rarely run because we may have an exceptional edge that
                // branches here.
                //
                if (bbIsHandlerBeg(block))
                {
                    rare = false;
                }
            }

            if (rare)
            {
                block->bbSetRunRarely();
                result = true;

#ifdef DEBUG
                if (verbose)
                {
                    printf("All branches to " FMT_BB " are from rarely run blocks, marking as rarely run\n",
                           block->bbNum);
                }
#endif // DEBUG

                // When marking a BBJ_CALLFINALLY as rarely run we also mark
                // the BBJ_CALLFINALLYRET that comes after it as rarely run
                //
                if (block->isBBCallFinallyPair())
                {
                    BasicBlock* bNext = block->Next();
                    assert(bNext != nullptr);
                    bNext->bbSetRunRarely();
#ifdef DEBUG
                    if (verbose)
                    {
                        printf("Also marking the BBJ_CALLFINALLYRET at " FMT_BB " as rarely run\n", bNext->bbNum);
                    }
#endif // DEBUG
                }
            }
        }

        //
        // if bPrev->bbWeight is not based upon profile data we can adjust
        // the weights of bPrev and block
        //
        if (bPrev->isBBCallFinallyPair() &&         // we must have a BBJ_CALLFINALLY and BBJ_CALLFINALLYRET pair
            (bPrev->bbWeight != block->bbWeight) && // the weights are currently different
            !bPrev->hasProfileWeight())             // and the BBJ_CALLFINALLY block is not using profiled weights
        {
            if (block->isRunRarely())
            {
                // Set the BBJ_CALLFINALLY block to the same weight as the BBJ_CALLFINALLYRET block and
                // mark it rarely run.
                bPrev->bbWeight = block->bbWeight;
#ifdef DEBUG
                if (verbose)
                {
                    printf("Marking the BBJ_CALLFINALLY block at " FMT_BB " as rarely run because " FMT_BB
                           " is rarely run\n",
                           bPrev->bbNum, block->bbNum);
                }
#endif // DEBUG
            }
            else if (bPrev->isRunRarely())
            {
                // Set the BBJ_CALLFINALLYRET block to the same weight as the BBJ_CALLFINALLY block and
                // mark it rarely run.
                block->bbWeight = bPrev->bbWeight;
#ifdef DEBUG
                if (verbose)
                {
                    printf("Marking the BBJ_CALLFINALLYRET block at " FMT_BB " as rarely run because " FMT_BB
                           " is rarely run\n",
                           block->bbNum, bPrev->bbNum);
                }
#endif // DEBUG
            }
            else // Both blocks are hot, bPrev is known not to be using profiled weight
            {
                // Set the BBJ_CALLFINALLY block to the same weight as the BBJ_CALLFINALLYRET block
                bPrev->bbWeight = block->bbWeight;
            }
            noway_assert(block->bbWeight == bPrev->bbWeight);
        }
    }

    return result;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::EdgeCmp: Comparator for the 'cutPoints' priority queue.
// If 'left' has a bigger edge weight than 'right', 3-opt will consider it first.
// Else, 3-opt will consider 'right' first.
//
// Parameters:
//   left - One of the two edges to compare
//   right - The other edge to compare
//
// Returns:
//   True if 'right' should be considered before 'left', and false otherwise
//
template <bool hasEH>
/* static */ bool Compiler::ThreeOptLayout<hasEH>::EdgeCmp(const FlowEdge* left, const FlowEdge* right)
{
    assert(left != right);
    const weight_t leftWeight  = left->getLikelyWeight();
    const weight_t rightWeight = right->getLikelyWeight();

    // Break ties by comparing the source blocks' bbIDs.
    // If both edges are out of the same source block, use the target blocks' bbIDs.
    if (leftWeight == rightWeight)
    {
        BasicBlock* const leftSrc  = left->getSourceBlock();
        BasicBlock* const rightSrc = right->getSourceBlock();
        if (leftSrc == rightSrc)
        {
            return left->getDestinationBlock()->bbID < right->getDestinationBlock()->bbID;
        }

        return leftSrc->bbID < rightSrc->bbID;
    }

    return leftWeight < rightWeight;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::ThreeOptLayout: Constructs a ThreeOptLayout instance.
//
// Parameters:
//   comp - The Compiler instance
//   initialLayout - An array of the blocks to be reordered
//   numHotBlocks - The number of hot blocks at the beginning of 'initialLayout'
//
// Notes:
//   To save an allocation, we will reuse the DFS tree's underlying array for 'tempOrder'.
//   This means we will trash the DFS tree.
//
template <bool hasEH>
Compiler::ThreeOptLayout<hasEH>::ThreeOptLayout(Compiler* comp, BasicBlock** initialLayout, unsigned numHotBlocks)
    : compiler(comp)
    , cutPoints(comp->getAllocator(CMK_FlowEdge), &ThreeOptLayout::EdgeCmp)
    , blockOrder(initialLayout)
    , tempOrder(comp->m_dfsTree->GetPostOrder())
    , numCandidateBlocks(numHotBlocks)
{
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::IsCandidateBlock: Determines if a block is being considered for reordering
// by checking if it is in 'blockOrder'.
//
// Parameters:
//   block - the block to check
//
// Returns:
//   True if 'block' is in the set of candidate blocks, false otherwise
//
template <bool hasEH>
bool Compiler::ThreeOptLayout<hasEH>::IsCandidateBlock(BasicBlock* block) const
{
    assert(block != nullptr);
    return (block->bbPreorderNum < numCandidateBlocks) && (blockOrder[block->bbPreorderNum] == block);
}

#ifdef DEBUG
//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::GetLayoutCost: Computes the cost of the layout for the region
// bounded by 'startPos' and 'endPos'.
//
// Parameters:
//   startPos - The starting index of the region
//   endPos - The inclusive ending index of the region
//
// Returns:
//   The region's layout cost
//
template <bool hasEH>
weight_t Compiler::ThreeOptLayout<hasEH>::GetLayoutCost(unsigned startPos, unsigned endPos)
{
    assert(startPos <= endPos);
    assert(endPos < numCandidateBlocks);
    weight_t layoutCost = BB_ZERO_WEIGHT;

    for (unsigned position = startPos; position < endPos; position++)
    {
        layoutCost += GetCost(blockOrder[position], blockOrder[position + 1]);
    }

    layoutCost += blockOrder[endPos]->bbWeight;
    return layoutCost;
}
#endif // DEBUG

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::GetCost: Computes the cost of placing 'next' after 'block'.
// Layout cost is modeled as the sum of block weights, minus the weights of edges that fall through.
//
// Parameters:
//   block - The block to consider creating fallthrough from
//   next - The block to consider creating fallthrough into
//
// Returns:
//   The cost
//
template <bool hasEH>
weight_t Compiler::ThreeOptLayout<hasEH>::GetCost(BasicBlock* block, BasicBlock* next)
{
    assert(block != nullptr);
    assert(next != nullptr);

    const weight_t  maxCost         = block->bbWeight;
    const FlowEdge* fallthroughEdge = compiler->fgGetPredForBlock(next, block);

    if (fallthroughEdge != nullptr)
    {
        // The edge's weight should never exceed its source block's weight,
        // but handle negative results from rounding errors in getLikelyWeight(), just in case
        return max(0.0, maxCost - fallthroughEdge->getLikelyWeight());
    }

    return maxCost;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::GetPartitionCostDelta: Computes the current cost of the given partitions,
// and the cost of swapping S2 and S3, returning the difference between them.
//
// Parameters:
//   s2Start - The starting position of the second partition
//   s3Start - The starting position of the third partition
//   s3End - The ending position (inclusive) of the third partition
//   s4End - The ending position (inclusive) of the fourth partition
//
// Returns:
//   The difference in cost between the current and proposed layouts.
//   A negative delta indicates the proposed layout is an improvement.
//
template <bool hasEH>
weight_t Compiler::ThreeOptLayout<hasEH>::GetPartitionCostDelta(unsigned s2Start,
                                                                unsigned s3Start,
                                                                unsigned s3End,
                                                                unsigned s4End)
{
    BasicBlock* const s2Block     = blockOrder[s2Start];
    BasicBlock* const s2BlockPrev = blockOrder[s2Start - 1];
    BasicBlock* const s3Block     = blockOrder[s3Start];
    BasicBlock* const s3BlockPrev = blockOrder[s3Start - 1];
    BasicBlock* const lastBlock   = blockOrder[s3End];

    // Evaluate the cost of swapping S2 and S3
    weight_t currCost = GetCost(s2BlockPrev, s2Block) + GetCost(s3BlockPrev, s3Block);
    weight_t newCost  = GetCost(s2BlockPrev, s3Block) + GetCost(lastBlock, s2Block);

    // Consider flow into S4, if the partition exists
    if (s3End < s4End)
    {
        BasicBlock* const s4StartBlock = blockOrder[s3End + 1];
        currCost += GetCost(lastBlock, s4StartBlock);
        newCost += GetCost(s3BlockPrev, s4StartBlock);
    }
    else
    {
        assert(s3End == s4End);
        currCost += lastBlock->bbWeight;
        newCost += s3BlockPrev->bbWeight;
    }

    return newCost - currCost;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::SwapPartitions: Swap the specified partitions.
// It is assumed (and asserted) that the swap is profitable.
//
// Parameters:
//   s1Start - The starting position of the first partition
//   s2Start - The starting position of the second partition
//   s3Start - The starting position of the third partition
//   s3End - The ending position (inclusive) of the third partition
//   s4End - The ending position (inclusive) of the fourth partition
//
// Notes:
//   Here is the proposed partition:
//   S1: s1Start ~ s2Start-1
//   S2: s2Start ~ s3Start-1
//   S3: s3Start ~ s3End
//   S4: remaining blocks
//
//   After the swap:
//   S1: s1Start ~ s2Start-1
//   S3: s3Start ~ s3End
//   S2: s2Start ~ s3Start-1
//   S4: remaining blocks
//
//   If 's3End' and 's4End' are the same, the fourth partition doesn't exist.
//
template <bool hasEH>
void Compiler::ThreeOptLayout<hasEH>::SwapPartitions(
    unsigned s1Start, unsigned s2Start, unsigned s3Start, unsigned s3End, unsigned s4End)
{
    INDEBUG(const weight_t currLayoutCost = GetLayoutCost(s1Start, s4End));

    // Swap the partitions
    const unsigned     s1Size      = s2Start - s1Start;
    const unsigned     s2Size      = s3Start - s2Start;
    const unsigned     s3Size      = (s3End + 1) - s3Start;
    BasicBlock** const regionStart = blockOrder + s1Start;
    BasicBlock** const tempStart   = tempOrder + s1Start;
    memcpy(tempStart, regionStart, sizeof(BasicBlock*) * s1Size);
    memcpy(tempStart + s1Size, regionStart + s1Size + s2Size, sizeof(BasicBlock*) * s3Size);
    memcpy(tempStart + s1Size + s3Size, regionStart + s1Size, sizeof(BasicBlock*) * s2Size);

    // Copy remaining blocks in S4 over
    const unsigned numBlocks     = (s4End - s1Start) + 1;
    const unsigned swappedSize   = s1Size + s2Size + s3Size;
    const unsigned remainingSize = numBlocks - swappedSize;
    assert(numBlocks >= swappedSize);
    memcpy(tempStart + swappedSize, regionStart + swappedSize, sizeof(BasicBlock*) * remainingSize);

    std::swap(blockOrder, tempOrder);

    // Update the ordinals for the blocks we moved
    for (unsigned i = s2Start; i <= s4End; i++)
    {
        blockOrder[i]->bbPreorderNum = i;
    }

#ifdef DEBUG
    // Don't bother checking if the cost improved for exceptionally costly layouts.
    // Imprecision from summing large floating-point values can falsely trigger the below assert.
    constexpr weight_t maxLayoutCostToCheck = (weight_t)UINT32_MAX;
    if (currLayoutCost < maxLayoutCostToCheck)
    {
        // Ensure the swap improved the overall layout. Tolerate some imprecision.
        const weight_t newLayoutCost = GetLayoutCost(s1Start, s4End);
        assert((newLayoutCost < currLayoutCost) ||
               Compiler::fgProfileWeightsEqual(newLayoutCost, currLayoutCost, 0.001));
    }
#endif // DEBUG
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::ConsiderEdge: Adds 'edge' to 'cutPoints' for later consideration
// if 'edge' looks promising, and it hasn't been considered already.
// Since adding to 'cutPoints' has logarithmic time complexity and might cause a heap allocation,
// avoid adding edges that 3-opt obviously won't consider later.
//
// Parameters:
//   edge - The branch to consider creating fallthrough for
//
// Template parameters:
//   addToQueue - If true, adds valid edges to the 'cutPoints' queue
//
// Returns:
//   True if 'edge' can be considered for aligning, false otherwise
//
template <bool hasEH>
template <bool addToQueue>
bool Compiler::ThreeOptLayout<hasEH>::ConsiderEdge(FlowEdge* edge)
{
    assert(edge != nullptr);

    // Don't add an edge that we've already considered.
    // For exceptionally branchy methods, we want to avoid exploding 'cutPoints' in size.
    if (addToQueue && edge->visited())
    {
        return false;
    }

    BasicBlock* const srcBlk = edge->getSourceBlock();
    BasicBlock* const dstBlk = edge->getDestinationBlock();

    // Don't consider edges to or from outside the hot range.
    if (!IsCandidateBlock(srcBlk) || !IsCandidateBlock(dstBlk))
    {
        return false;
    }

    // Don't consider single-block loop backedges.
    if (srcBlk == dstBlk)
    {
        return false;
    }

    // Don't move the method entry block.
    if (dstBlk->IsFirst())
    {
        return false;
    }

    // Ignore cross-region branches, and don't try to change the region's entry block.
    if (hasEH && (!BasicBlock::sameTryRegion(srcBlk, dstBlk) || compiler->bbIsTryBeg(dstBlk)))
    {
        return false;
    }

    if (addToQueue)
    {
        edge->markVisited();
        cutPoints.Push(edge);
    }

    return true;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::AddNonFallthroughSuccs: Considers every edge out of a given block
// that doesn't fall through as a future cut point.
//
// Parameters:
//   blockPos - The index into 'blockOrder' of the source block
//
template <bool hasEH>
void Compiler::ThreeOptLayout<hasEH>::AddNonFallthroughSuccs(unsigned blockPos)
{
    assert(blockPos < numCandidateBlocks);
    BasicBlock* const block = blockOrder[blockPos];
    BasicBlock* const next  = ((blockPos + 1) >= numCandidateBlocks) ? nullptr : blockOrder[blockPos + 1];

    for (FlowEdge* const succEdge : block->SuccEdges())
    {
        if (succEdge->getDestinationBlock() != next)
        {
            ConsiderEdge(succEdge);
        }
    }
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::AddNonFallthroughPreds: Considers every edge into a given block
// that doesn't fall through as a future cut point.
//
// Parameters:
//   blockPos - The index into 'blockOrder' of the target block
//
template <bool hasEH>
void Compiler::ThreeOptLayout<hasEH>::AddNonFallthroughPreds(unsigned blockPos)
{
    assert(blockPos < numCandidateBlocks);
    BasicBlock* const block = blockOrder[blockPos];
    BasicBlock* const prev  = (blockPos == 0) ? nullptr : blockOrder[blockPos - 1];

    for (FlowEdge* const predEdge : block->PredEdges())
    {
        if (predEdge->getSourceBlock() != prev)
        {
            ConsiderEdge(predEdge);
        }
    }
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::Run: Runs 3-opt on the candidate span of hot blocks.
// We skip reordering handler regions for now, as these are assumed to be cold.
//
// Returns:
//   True if any blocks were moved
//
template <bool hasEH>
bool Compiler::ThreeOptLayout<hasEH>::Run()
{
    assert(numCandidateBlocks > 0);
    RunThreeOpt();
    return ReorderBlockList();
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::RunGreedyThreeOptPass: Runs 3-opt for the given block range,
// using a greedy strategy for finding partitions to swap.
//
// Parameters:
//   startBlock - The first block of the range to reorder
//   endBlock - The last block (inclusive) of the range to reorder
//
// Returns:
//   True if we reordered anything, false otherwise
//
// Notes:
//   For methods with more than a trivial number of basic blocks,
//   iteratively trying every cut point is prohibitively expensive.
//   Instead, add the non-fallthrough successor edges of each block to a priority queue,
//   and try to create fallthrough on each edge via partition swaps, starting with the hottest edges.
//   For each swap, repopulate the priority queue with edges along the modified cut points.
//
template <bool hasEH>
bool Compiler::ThreeOptLayout<hasEH>::RunGreedyThreeOptPass(unsigned startPos, unsigned endPos)
{
    assert(cutPoints.Empty());
    assert(startPos < endPos);
    bool modified = false;

    JITDUMP("Running greedy 3-opt pass.\n");

    // Initialize cutPoints with candidate branches in this section
    for (unsigned position = startPos; position <= endPos; position++)
    {
        AddNonFallthroughSuccs(position);
    }

    // For each candidate edge, determine if it's profitable to partition after the source block
    // and before the destination block, and swap the partitions to create fallthrough.
    // If it is, do the swap, and for the blocks before/after each cut point that lost fallthrough,
    // consider adding their successors/predecessors to 'cutPoints'.
    unsigned numSwaps = 0;
    while (!cutPoints.Empty() && (numSwaps < maxSwaps))
    {
        FlowEdge* const candidateEdge = cutPoints.Pop();
        candidateEdge->markUnvisited();

        BasicBlock* const srcBlk = candidateEdge->getSourceBlock();
        BasicBlock* const dstBlk = candidateEdge->getDestinationBlock();
        const unsigned    srcPos = srcBlk->bbPreorderNum;
        const unsigned    dstPos = dstBlk->bbPreorderNum;

        // This edge better be between blocks in the current region
        assert((srcPos >= startPos) && (srcPos <= endPos));
        assert((dstPos >= startPos) && (dstPos <= endPos));

        // 'dstBlk' better not be the region's entry point
        assert(dstPos != startPos);

        // 'srcBlk' and 'dstBlk' better be distinct
        assert(srcPos != dstPos);

        // Previous moves might have inadvertently created fallthrough from 'srcBlk' to 'dstBlk',
        // so there's nothing to do this round.
        if ((srcPos + 1) == dstPos)
        {
            assert(modified);
            continue;
        }

        // Before getting any edges, make sure the ordinals are accurate
        assert(blockOrder[srcPos] == srcBlk);
        assert(blockOrder[dstPos] == dstBlk);

        // To determine if it's worth creating fallthrough from 'srcBlk' into 'dstBlk',
        // we first determine the current layout cost at the proposed cut points.
        // We then compare this to the layout cost with the partitions swapped.
        // If the new cost improves upon the current cost, then we can justify the swap.

        const bool isForwardJump = (srcPos < dstPos);
        unsigned   s2Start, s3Start, s3End;
        weight_t   costChange;

        if (isForwardJump)
        {
            // Here is the proposed partition:
            // S1: startPos ~ srcPos
            // S2: srcPos+1 ~ dstPos-1
            // S3: dstPos ~ endPos
            // S4: remaining blocks
            //
            // After the swap:
            // S1: startPos ~ srcPos
            // S3: dstPos ~ endPos
            // S2: srcPos+1 ~ dstPos-1
            // S4: remaining blocks
            s2Start    = srcPos + 1;
            s3Start    = dstPos;
            s3End      = endPos;
            costChange = GetPartitionCostDelta(s2Start, s3Start, s3End, endPos);
        }
        else
        {
            // For backward jumps, we will employ a greedy 4-opt approach to find the ideal cut point
            // between the destination and source blocks.
            // Here is the proposed partition:
            // S1: startPos ~ dstPos-1
            // S2: dstPos ~ s3Start-1
            // S3: s3Start ~ srcPos
            // S4: srcPos+1 ~ endPos
            //
            // After the swap:
            // S1: startPos ~ dstPos-1
            // S3: s3Start ~ srcPos
            // S2: dstPos ~ s3Start-1
            // S4: srcPos+1 ~ endPos
            s2Start    = dstPos;
            s3Start    = srcPos;
            s3End      = srcPos;
            costChange = BB_ZERO_WEIGHT;

            // The cut points before S2 and after S3 are fixed.
            // We will search for the optimal cut point before S3.
            BasicBlock* const s2Block     = blockOrder[s2Start];
            BasicBlock* const s2BlockPrev = blockOrder[s2Start - 1];
            BasicBlock* const lastBlock   = blockOrder[s3End];

            // Because the above cut points are fixed, don't waste time re-computing their costs.
            // Instead, pre-compute them here.
            const weight_t currCostBase =
                GetCost(s2BlockPrev, s2Block) +
                ((s3End < endPos) ? GetCost(lastBlock, blockOrder[s3End + 1]) : lastBlock->bbWeight);
            const weight_t newCostBase = GetCost(lastBlock, s2Block);

            // Search for the ideal start to S3
            for (unsigned position = s2Start + 1; position <= s3End; position++)
            {
                BasicBlock* const s3Block     = blockOrder[position];
                BasicBlock* const s3BlockPrev = blockOrder[position - 1];

                // Don't consider any cut points that would break up call-finally pairs
                if (hasEH && s3Block->KindIs(BBJ_CALLFINALLYRET))
                {
                    continue;
                }

                // Compute the cost delta of this partition
                const weight_t currCost = currCostBase + GetCost(s3BlockPrev, s3Block);
                const weight_t newCost =
                    newCostBase + GetCost(s2BlockPrev, s3Block) +
                    ((s3End < endPos) ? GetCost(s3BlockPrev, blockOrder[s3End + 1]) : s3BlockPrev->bbWeight);
                const weight_t delta = newCost - currCost;

                if (delta < costChange)
                {
                    costChange = delta;
                    s3Start    = position;
                }
            }
        }

        // Continue evaluating partitions if this one isn't profitable
        if ((costChange >= BB_ZERO_WEIGHT) || Compiler::fgProfileWeightsEqual(costChange, BB_ZERO_WEIGHT, 0.001))
        {
            continue;
        }

        JITDUMP("Swapping partitions [" FMT_BB ", " FMT_BB "] and [" FMT_BB ", " FMT_BB "] (cost change = %f)\n",
                blockOrder[s2Start]->bbNum, blockOrder[s3Start - 1]->bbNum, blockOrder[s3Start]->bbNum,
                blockOrder[s3End]->bbNum, costChange);

        SwapPartitions(startPos, s2Start, s3Start, s3End, endPos);

        // Ensure this move created fallthrough from 'srcBlk' to 'dstBlk'
        assert((srcBlk->bbPreorderNum + 1) == dstBlk->bbPreorderNum);

        // At every cut point is an opportunity to consider more candidate edges.
        // To the left of each cut point, consider successor edges that don't fall through.
        // Ditto predecessor edges to the right of each cut point.
        AddNonFallthroughSuccs(s2Start - 1);
        AddNonFallthroughPreds(s2Start);
        AddNonFallthroughSuccs(s3Start - 1);
        AddNonFallthroughPreds(s3Start);
        AddNonFallthroughSuccs(s3End);

        if (s3End < endPos)
        {
            AddNonFallthroughPreds(s3End + 1);
        }

        modified = true;
        numSwaps++;
    }

    cutPoints.Clear();
    return modified;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::RunThreeOpt: Runs 3-opt on the candidate span of blocks.
//
template <bool hasEH>
void Compiler::ThreeOptLayout<hasEH>::RunThreeOpt()
{
    // For methods with fewer than three candidate blocks, we cannot partition anything
    if (numCandidateBlocks < 3)
    {
        JITDUMP("Not enough blocks to partition anything. Skipping reordering.\n");
        return;
    }

    CompactHotJumps();

    const unsigned startPos = 0;
    const unsigned endPos   = numCandidateBlocks - 1;

    JITDUMP("Initial layout cost: %f\n", GetLayoutCost(startPos, endPos));
    const bool modified = RunGreedyThreeOptPass(startPos, endPos);

    if (modified)
    {
        JITDUMP("Final layout cost: %f\n", GetLayoutCost(startPos, endPos));
    }
    else
    {
        JITDUMP("No changes made.\n");
    }
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::ReorderBlockList: Reorders blocks within their regions
// using the order 3-opt came up with.
// If the method has try regions, this will also move them to try to create fallthrough into their entries.
//
// Returns:
//   True if any blocks were moved
//
template <bool hasEH>
bool Compiler::ThreeOptLayout<hasEH>::ReorderBlockList()
{
    // As we reorder blocks, remember the last candidate block we found in each region.
    // In case we cannot place two blocks next to each other because they are in different regions,
    // we will instead place the latter block after the last one we saw in its region.
    // This ensures cold blocks sink to the end of their respective regions.
    // This will also push nested regions further down the method, but we will move them later, anyway.
    BasicBlock** lastHotBlocks = nullptr;

    if (hasEH)
    {
        lastHotBlocks    = new (compiler, CMK_BasicBlock) BasicBlock* [compiler->compHndBBtabCount + 1] {};
        lastHotBlocks[0] = compiler->fgFirstBB;

        for (EHblkDsc* const HBtab : EHClauses(compiler))
        {
            lastHotBlocks[HBtab->ebdTryBeg->bbTryIndex] = HBtab->ebdTryBeg;
        }
    }

    // Reorder the block list.
    JITDUMP("Reordering block list\n");
    bool modified = false;
    for (unsigned i = 1; i < numCandidateBlocks; i++)
    {
        BasicBlock* const block       = blockOrder[i - 1];
        BasicBlock* const blockToMove = blockOrder[i];

        if (!hasEH)
        {
            if (!block->NextIs(blockToMove))
            {
                compiler->fgUnlinkBlock(blockToMove);
                compiler->fgInsertBBafter(block, blockToMove);
                modified = true;
            }

            continue;
        }

        lastHotBlocks[block->bbTryIndex] = block;

        // Don't move call-finally pair tails independently.
        // When we encounter the head, we will move the entire pair.
        if (blockToMove->isBBCallFinallyPairTail())
        {
            continue;
        }

        // Only reorder blocks within the same try region. We don't want to make them non-contiguous.
        if (compiler->bbIsTryBeg(blockToMove))
        {
            continue;
        }

        // If these blocks aren't in the same try region, use the last block seen in the same region as 'blockToMove'
        // for the insertion point.
        // This will push the region containing 'block' down the method, but we will fix this after.
        BasicBlock* insertionPoint =
            BasicBlock::sameTryRegion(block, blockToMove) ? block : lastHotBlocks[blockToMove->bbTryIndex];

        // Don't break up call-finally pairs by inserting something in the middle.
        if (insertionPoint->isBBCallFinallyPair())
        {
            insertionPoint = insertionPoint->Next();
            assert(blockToMove != insertionPoint);
        }

        if (insertionPoint->NextIs(blockToMove))
        {
            continue;
        }

        // Move call-finallies together.
        if (blockToMove->isBBCallFinallyPair())
        {
            BasicBlock* const callFinallyRet = blockToMove->Next();
            if (callFinallyRet != insertionPoint)
            {
                compiler->fgUnlinkRange(blockToMove, callFinallyRet);
                compiler->fgMoveBlocksAfter(blockToMove, callFinallyRet, insertionPoint);
                modified = true;
            }
        }
        else
        {
            compiler->fgUnlinkBlock(blockToMove);
            compiler->fgInsertBBafter(insertionPoint, blockToMove);
            modified = true;
        }
    }

    if (!hasEH)
    {
        return modified;
    }

    // If we reordered within any try regions, make sure the EH table is up-to-date.
    if (modified)
    {
        compiler->fgFindTryRegionEnds();
    }

    JITDUMP("Moving try regions\n");

    // We only ordered blocks within regions above.
    // Now, move entire try regions up to their ideal predecessors, if possible.
    for (EHblkDsc* const HBtab : EHClauses(compiler))
    {
        // If this try region isn't in the candidate span of blocks, don't consider it.
        // Also, if this try region's entry is also the method entry, don't move it.
        BasicBlock* const tryBeg = HBtab->ebdTryBeg;
        if (!IsCandidateBlock(tryBeg) || tryBeg->IsFirst())
        {
            continue;
        }

        // We will try using 3-opt's chosen predecessor for the try region.
        BasicBlock*    insertionPoint = blockOrder[tryBeg->bbPreorderNum - 1];
        const unsigned parentIndex =
            insertionPoint->hasTryIndex() ? insertionPoint->getTryIndex() : EHblkDsc::NO_ENCLOSING_INDEX;

        // Can we move this try to after 'insertionPoint' without breaking EH nesting invariants?
        if (parentIndex != HBtab->ebdEnclosingTryIndex)
        {
            // We cannot.
            continue;
        }

        // Don't break up call-finally pairs.
        if (insertionPoint->isBBCallFinallyPair())
        {
            insertionPoint = insertionPoint->Next();
        }

        // Nothing to do if we already fall through.
        if (insertionPoint->NextIs(tryBeg))
        {
            continue;
        }

        BasicBlock* const tryLast = HBtab->ebdTryLast;
        compiler->fgUnlinkRange(tryBeg, tryLast);
        compiler->fgMoveBlocksAfter(tryBeg, tryLast, insertionPoint);
        modified = true;

        // If we moved this region within another region, recompute the try region end blocks.
        if (parentIndex != EHblkDsc::NO_ENCLOSING_INDEX)
        {
            compiler->fgFindTryRegionEnds();
        }
    }

    return modified;
}

//-----------------------------------------------------------------------------
// Compiler::ThreeOptLayout::CompactHotJumps: Move blocks in the candidate span
// closer to their most-likely successors.
//
template <bool hasEH>
void Compiler::ThreeOptLayout<hasEH>::CompactHotJumps()
{
    JITDUMP("Compacting hot jumps\n");

    auto isBackwardJump = [&](BasicBlock* block, BasicBlock* target) {
        assert(IsCandidateBlock(block));
        assert(IsCandidateBlock(target));
        return block->bbPreorderNum >= target->bbPreorderNum;
    };

    for (unsigned i = 0; i < numCandidateBlocks; i++)
    {
        BasicBlock* const block = blockOrder[i];
        FlowEdge*         edge;
        FlowEdge*         unlikelyEdge;

        if (block->KindIs(BBJ_ALWAYS))
        {
            edge         = block->GetTargetEdge();
            unlikelyEdge = nullptr;
        }
        else if (block->KindIs(BBJ_COND))
        {
            // Consider conditional block's most likely branch for moving.
            if (block->GetTrueEdge()->getLikelihood() > 0.5)
            {
                edge         = block->GetTrueEdge();
                unlikelyEdge = block->GetFalseEdge();
            }
            else
            {
                edge         = block->GetFalseEdge();
                unlikelyEdge = block->GetTrueEdge();
            }

            // If we aren't sure which successor is hotter, and we already fall into one of them,
            // do nothing.
            BasicBlock* const unlikelyTarget = unlikelyEdge->getDestinationBlock();
            if ((unlikelyEdge->getLikelihood() == 0.5) && IsCandidateBlock(unlikelyTarget) &&
                (unlikelyTarget->bbPreorderNum == (i + 1)))
            {
                continue;
            }
        }
        else
        {
            // Don't consider other block kinds.
            continue;
        }

        // Ensure we won't break any ordering invariants by creating fallthrough on this edge.
        if (!ConsiderEdge</* addToQueue */ false>(edge))
        {
            continue;
        }

        if (block->KindIs(BBJ_COND) && isBackwardJump(block, edge->getDestinationBlock()))
        {
            // This could be a loop exit, so don't bother moving this block up.
            // Instead, try moving the unlikely target up to create fallthrough.
            if (!ConsiderEdge</* addToQueue */ false>(unlikelyEdge) ||
                isBackwardJump(block, unlikelyEdge->getDestinationBlock()))
            {
                continue;
            }

            edge = unlikelyEdge;
        }

        BasicBlock* const target = edge->getDestinationBlock();
        const unsigned    srcPos = i;
        const unsigned    dstPos = target->bbPreorderNum;

        // We don't need to do anything if this edge already falls through.
        if ((srcPos + 1) == dstPos)
        {
            continue;
        }

        // If this move will break up existing fallthrough into 'target', make sure it's worth it.
        assert(dstPos != 0);
        FlowEdge* const fallthroughEdge = compiler->fgGetPredForBlock(target, blockOrder[dstPos - 1]);
        if ((fallthroughEdge != nullptr) && (fallthroughEdge->getLikelyWeight() >= edge->getLikelyWeight()))
        {
            continue;
        }

        JITDUMP("Creating fallthrough along " FMT_BB " -> " FMT_BB "\n", block->bbNum, target->bbNum);

        const bool isForwardJump = !isBackwardJump(block, target);
        if (isForwardJump)
        {
            // Before swap: | ..srcBlk | ... | dstBlk | ... |
            // After swap:  | ..srcBlk | dstBlk | ... |

            // First, shift all blocks between 'block' and 'target' rightward to make space for the latter.
            // If 'target' is a call-finally pair, include space for the pair's tail.
            const unsigned offset = target->isBBCallFinallyPair() ? 2 : 1;
            for (unsigned pos = dstPos - 1; pos != srcPos; pos--)
            {
                BasicBlock* const blockToMove = blockOrder[pos];
                blockOrder[pos + offset]      = blockOrder[pos];
                blockToMove->bbPreorderNum += offset;
            }

            // Now, insert 'target' in the space after 'block'.
            blockOrder[srcPos + 1] = target;
            target->bbPreorderNum  = srcPos + 1;

            // Move call-finally pairs in tandem.
            if (target->isBBCallFinallyPair())
            {
                blockOrder[srcPos + 2]        = target->Next();
                target->Next()->bbPreorderNum = srcPos + 2;
            }
        }
        else
        {
            // Before swap: | ... | dstBlk.. | srcBlk | ... |
            // After swap:  | ... | srcBlk | dstBlk.. | ... |

            // First, shift everything between 'target' and 'block' (including 'target') over
            // to make space for 'block'.
            for (unsigned pos = srcPos - 1; pos >= dstPos; pos--)
            {
                BasicBlock* const blockToMove = blockOrder[pos];
                blockOrder[pos + 1]           = blockOrder[pos];
                blockToMove->bbPreorderNum++;
            }

            // Now, insert 'block' before 'target'.
            blockOrder[dstPos]   = block;
            block->bbPreorderNum = dstPos;
        }

        assert((block->bbPreorderNum + 1) == target->bbPreorderNum);
    }
}

//-----------------------------------------------------------------------------
// fgSearchImprovedLayout: Try to improve upon RPO-based layout with the 3-opt method:
//   - Identify a range of hot blocks to reorder within
//   - Partition this set into three segments: S1 - S2 - S3
//   - Evaluate cost of swapped layout: S1 - S3 - S2
//   - If the cost improves, keep this layout
//
// Returns:
//   Suitable phase status
//
PhaseStatus Compiler::fgSearchImprovedLayout()
{
#ifdef DEBUG
    if (verbose)
    {
        printf("*************** In fgSearchImprovedLayout()\n");

        printf("\nInitial BasicBlocks");
        fgDispBasicBlocks(verboseTrees);
        printf("\n");
    }
#endif // DEBUG

    // Before running 3-opt, compute a loop-aware RPO (if not already available) to get a sensible starting layout.
    if (m_dfsTree == nullptr)
    {
        m_dfsTree = fgComputeDfs</* useProfile */ true>();
        m_loops   = FlowGraphNaturalLoops::Find(m_dfsTree);
    }
    else
    {
        assert(m_loops != nullptr);
    }

    BasicBlock** const initialLayout = new (this, CMK_BasicBlock) BasicBlock*[m_dfsTree->GetPostOrderCount()];

    // When walking the RPO-based layout, compact the hot blocks, and remember the end of the hot section.
    // We don't want to waste time running 3-opt on cold blocks, or on handler sections.
    unsigned numHotBlocks  = 0;
    auto     addToSequence = [this, initialLayout, &numHotBlocks](BasicBlock* block) {
        // The first block really shouldn't be cold, but if it is, ensure it's still placed first.
        if (!block->hasHndIndex() && (!block->isBBWeightCold(this) || block->IsFirst()))
        {
            // Set the block's ordinal.
            block->bbPreorderNum          = numHotBlocks;
            initialLayout[numHotBlocks++] = block;
        }
    };

    // Stress 3-opt by giving it the post-order traversal as its initial layout.
    if (compStressCompile(STRESS_THREE_OPT_LAYOUT, 10))
    {
        for (unsigned i = 0; i < m_dfsTree->GetPostOrderCount(); i++)
        {
            addToSequence(m_dfsTree->GetPostOrder(i));
        }

        // Keep the method entry block at the beginning.
        // Update the swapped blocks' ordinals, too.
        std::swap(initialLayout[0], initialLayout[numHotBlocks - 1]);
        std::swap(initialLayout[0]->bbPreorderNum, initialLayout[numHotBlocks - 1]->bbPreorderNum);
    }
    else
    {
        fgVisitBlocksInLoopAwareRPO(m_dfsTree, m_loops, addToSequence);
    }

    bool modified = false;
    if (numHotBlocks == 0)
    {
        JITDUMP("No hot blocks found. Skipping reordering.\n");
    }
    else if (compHndBBtabCount == 0)
    {
        ThreeOptLayout</* hasEH */ false> layoutRunner(this, initialLayout, numHotBlocks);
        modified = layoutRunner.Run();
    }
    else
    {
        ThreeOptLayout</* hasEH */ true> layoutRunner(this, initialLayout, numHotBlocks);
        modified = layoutRunner.Run();
    }

    // 3-opt will mess with post-order numbers regardless of whether it modifies anything,
    // so we always need to invalidate the flowgraph annotations after.
    fgInvalidateDfsTree();
    return modified ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//-------------------------------------------------------------
// fgUpdateFlowGraphPhase: run flow graph optimization as a
//   phase, with no tail duplication
//
// Returns:
//    Suitable phase status
//
PhaseStatus Compiler::fgUpdateFlowGraphPhase()
{
    constexpr bool doTailDup   = false;
    constexpr bool isPhase     = true;
    const bool     madeChanges = fgUpdateFlowGraph(doTailDup, isPhase);

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//-------------------------------------------------------------
// fgDedupReturnComparison: Expands BBJ_RETURN <relop> into BBJ_COND <relop> with two
//   BBJ_RETURN blocks ("return true" and "return false"). Such transformation
//   helps other phases to focus only on BBJ_COND <relop> (normalization).
//
// Arguments:
//    block - the BBJ_RETURN block to convert into BBJ_COND <relop>
//
// Returns:
//    true if the block was converted into BBJ_COND <relop>
//
bool Compiler::fgDedupReturnComparison(BasicBlock* block)
{
#ifdef JIT32_GCENCODER
    // JIT32_GCENCODER has a hard limit on the number of epilogues, let's not add more.
    return false;
#endif

    assert(block->KindIs(BBJ_RETURN));

    // We're only interested in boolean returns
    if ((info.compRetType != TYP_UBYTE) || (block == genReturnBB) || (block->lastStmt() == nullptr))
    {
        return false;
    }

    GenTree* rootNode = block->lastStmt()->GetRootNode();
    if (!rootNode->OperIs(GT_RETURN) || !rootNode->gtGetOp1()->OperIsCmpCompare())
    {
        return false;
    }

    GenTree* cmp = rootNode->gtGetOp1();
    cmp->gtFlags |= (GTF_RELOP_JMP_USED | GTF_DONT_CSE);
    rootNode->ChangeOper(GT_JTRUE);
    rootNode->ChangeType(TYP_VOID);

    GenTree* retTrue  = gtNewOperNode(GT_RETURN, TYP_INT, gtNewTrue());
    GenTree* retFalse = gtNewOperNode(GT_RETURN, TYP_INT, gtNewFalse());

    // Create RETURN 1/0 blocks. We expect fgHeadTailMerge to handle them if there are similar returns.
    DebugInfo   dbgInfo    = block->lastStmt()->GetDebugInfo();
    BasicBlock* retTrueBb  = fgNewBBFromTreeAfter(BBJ_RETURN, block, retTrue, dbgInfo);
    BasicBlock* retFalseBb = fgNewBBFromTreeAfter(BBJ_RETURN, block, retFalse, dbgInfo);

    FlowEdge* trueEdge  = fgAddRefPred(retTrueBb, block);
    FlowEdge* falseEdge = fgAddRefPred(retFalseBb, block);
    block->SetCond(trueEdge, falseEdge);

    // We might want to instrument 'return <cond>' too in the future. For now apply 50%/50%.
    trueEdge->setLikelihood(0.5);
    falseEdge->setLikelihood(0.5);
    retTrueBb->inheritWeightPercentage(block, 50);
    retFalseBb->inheritWeightPercentage(block, 50);

    return true;
}

//-------------------------------------------------------------
// fgUpdateFlowGraph: Removes any empty blocks, unreachable blocks, and redundant jumps.
// Most of those appear after dead store removal and folding of conditionals.
// Also, compact consecutive basic blocks.
//
// Arguments:
//    doTailDuplication - true to attempt tail duplication optimization
//    isPhase - true if being run as the only thing in a phase
//
// Returns: true if the flowgraph has been modified
//
// Notes:
//    Debuggable code and Min Optimization JIT also introduces basic blocks
//    but we do not optimize those!
//
bool Compiler::fgUpdateFlowGraph(bool doTailDuplication /* = false */, bool isPhase /* = false */)
{
#ifdef DEBUG
    if (verbose && !isPhase)
    {
        printf("\n*************** In fgUpdateFlowGraph()");
    }
#endif // DEBUG

    /* This should never be called for debuggable code */

    noway_assert(opts.OptimizationEnabled());

    // We shouldn't be churning the flowgraph after doing hot/cold splitting
    assert(fgFirstColdBlock == nullptr);

#ifdef DEBUG
    if (verbose && !isPhase)
    {
        printf("\nBefore updating the flow graph:\n");
        fgDispBasicBlocks(verboseTrees);
        printf("\n");
    }
#endif // DEBUG

    /* Walk all the basic blocks - look for unconditional jumps, empty blocks, blocks to compact, etc...
     *
     * OBSERVATION:
     *      Once a block is removed the predecessors are not accurate (assuming they were at the beginning)
     *      For now we will only use the information in bbRefs because it is easier to be updated
     */

    bool modified = false;
    bool change;
    do
    {
        change = false;

        BasicBlock* block;           // the current block
        BasicBlock* bPrev = nullptr; // the previous non-worthless block
        BasicBlock* bNext;           // the successor of the current block
        BasicBlock* bDest;           // the jump target of the current block
        BasicBlock* bFalseDest;      // the false target of the current block (if it is a BBJ_COND)

        for (block = fgFirstBB; block != nullptr; block = block->Next())
        {
            /*  Some blocks may be already marked removed by other optimizations
             *  (e.g worthless loop removal), without being explicitly removed
             *  from the list.
             */

            if (block->HasFlag(BBF_REMOVED))
            {
                if (bPrev)
                {
                    assert(!block->IsLast());
                    bPrev->SetNext(block->Next());
                }
                else
                {
                    /* WEIRD first basic block is removed - should have an assert here */
                    noway_assert(!"First basic block marked as BBF_REMOVED???");

                    fgFirstBB = block->Next();
                }
                continue;
            }

            /*  We jump to the REPEAT label if we performed a change involving the current block
             *  This is in case there are other optimizations that can show up
             *  (e.g. - compact 3 blocks in a row)
             *  If nothing happens, we then finish the iteration and move to the next block
             */

        REPEAT:;

            bNext      = block->Next();
            bDest      = nullptr;
            bFalseDest = nullptr;

            // Expand BBJ_RETURN <relop> into BBJ_COND <relop> when doTailDuplication is enabled
            if (doTailDuplication && block->KindIs(BBJ_RETURN) && fgDedupReturnComparison(block))
            {
                assert(block->KindIs(BBJ_COND));
                change   = true;
                modified = true;
                bNext    = block->Next();
            }

            if (block->KindIs(BBJ_ALWAYS))
            {
                bDest = block->GetTarget();
                if (doTailDuplication && fgOptimizeUncondBranchToSimpleCond(block, bDest))
                {
                    assert(block->KindIs(BBJ_COND));
                    assert(bNext == block->Next());
                    change   = true;
                    modified = true;

                    if (fgFoldSimpleCondByForwardSub(block))
                    {
                        // It is likely another pred of the target now can
                        // similarly have its control flow straightened out.
                        // Try to compact it and repeat the optimization for
                        // it.
                        if (bDest->bbRefs == 1)
                        {
                            BasicBlock* otherPred = bDest->bbPreds->getSourceBlock();
                            JITDUMP("Trying to compact last pred " FMT_BB " of " FMT_BB " that we now bypass\n",
                                    otherPred->bbNum, bDest->bbNum);
                            if (fgCanCompactBlock(otherPred))
                            {
                                fgCompactBlock(otherPred);
                                fgFoldSimpleCondByForwardSub(otherPred);

                                // Since compaction removes blocks, update lexical pointers
                                bPrev = block->Prev();
                                bNext = block->Next();
                            }
                        }

                        assert(block->KindIs(BBJ_ALWAYS));
                        bDest = block->GetTarget();
                    }
                }
            }

            // Remove jumps to the following block and optimize any JUMPS to JUMPS

            if (block->KindIs(BBJ_ALWAYS, BBJ_CALLFINALLYRET))
            {
                bDest = block->GetTarget();
                if (bDest == bNext)
                {
                    // Skip jump optimizations, and try to compact block and bNext later
                    bDest = nullptr;
                }
            }
            else if (block->KindIs(BBJ_COND))
            {
                bDest      = block->GetTrueTarget();
                bFalseDest = block->GetFalseTarget();
                if (bDest == bFalseDest)
                {
                    fgRemoveConditionalJump(block);
                    assert(block->KindIs(BBJ_ALWAYS));
                    change     = true;
                    modified   = true;
                    bFalseDest = nullptr;
                }
            }

            if (bDest != nullptr)
            {
                // Do we have a JUMP to an empty unconditional JUMP block?
                if (bDest->KindIs(BBJ_ALWAYS) && !bDest->TargetIs(bDest) && // special case for self jumps
                    bDest->isEmpty())
                {
                    // Empty blocks that jump to the next block can probably be compacted instead
                    if (!bDest->JumpsToNext() && fgOptimizeBranchToEmptyUnconditional(block, bDest))
                    {
                        change   = true;
                        modified = true;
                        goto REPEAT;
                    }
                }

                // Check for cases where reversing the branch condition may enable
                // other flow opts.
                //
                // Current block falls through to an empty bNext BBJ_ALWAYS, and
                // (a) block jump target is bNext's bbNext.
                // (b) block jump target is elsewhere but join free, and
                //      bNext's jump target has a join.
                //
                if (block->KindIs(BBJ_COND) &&   // block is a BBJ_COND block
                    (bFalseDest == bNext) &&     // false target is the next block
                    (bNext->bbRefs == 1) &&      // no other block jumps to bNext
                    bNext->KindIs(BBJ_ALWAYS) && // the next block is a BBJ_ALWAYS block
                    !bNext->JumpsToNext() &&     // and it doesn't jump to the next block (we might compact them)
                    bNext->isEmpty() &&          // and it is an empty block
                    !bNext->TargetIs(bNext))     // special case for self jumps
                {
                    assert(block->FalseTargetIs(bNext));

                    // case (a)
                    //
                    const bool isJumpAroundEmpty = bNext->NextIs(bDest);

                    // case (b)
                    //
                    // Note the asymmetric checks for refs == 1 and refs > 1 ensures that we
                    // differentiate the roles played by bDest and bNextJumpDest. We need some
                    // sense of which arrangement is preferable to avoid getting stuck in a loop
                    // reversing and re-reversing.
                    //
                    // Other tiebreaking criteria could be considered.
                    //
                    // Pragmatic constraints:
                    //
                    // * don't consider lexical predecessors, or we may confuse loop recognition
                    // * don't consider blocks of different rarities
                    //
                    BasicBlock* const bNextJumpDest    = bNext->GetTarget();
                    const bool        isJumpToJoinFree = !isJumpAroundEmpty && (bDest->bbRefs == 1) &&
                                                  (bNextJumpDest->bbRefs > 1) && (bDest->bbNum > block->bbNum) &&
                                                  (block->isRunRarely() == bDest->isRunRarely());

                    bool optimizeJump = isJumpAroundEmpty || isJumpToJoinFree;

                    // We do not optimize jumps between two different try regions.
                    // However jumping to a block that is not in any try region is OK
                    //
                    if (bDest->hasTryIndex() && !BasicBlock::sameTryRegion(block, bDest))
                    {
                        optimizeJump = false;
                    }

                    // Also consider bNext's try region
                    //
                    if (bNext->hasTryIndex() && !BasicBlock::sameTryRegion(block, bNext))
                    {
                        optimizeJump = false;
                    }

                    if (optimizeJump && isJumpToJoinFree)
                    {
                        // In the join free case, we also need to move bDest right after bNext
                        // to create same flow as in the isJumpAroundEmpty case.
                        // However, we cannot move bDest if it will break EH invariants.
                        //
                        if (!BasicBlock::sameEHRegion(bNext, bDest) || bbIsTryBeg(bDest) || bbIsHandlerBeg(bDest) ||
                            bDest->isBBCallFinallyPair())
                        {
                            optimizeJump = false;
                        }
                        else
                        {
                            // We don't expect bDest to already be right after bNext.
                            //
                            assert(!bNext->NextIs(bDest));

                            JITDUMP("\nMoving " FMT_BB " after " FMT_BB " to enable reversal\n", bDest->bbNum,
                                    bNext->bbNum);

                            // Move bDest
                            //
                            if (ehIsBlockEHLast(bDest))
                            {
                                ehUpdateLastBlocks(bDest, bDest->Prev());
                            }

                            fgUnlinkBlock(bDest);
                            fgInsertBBafter(bNext, bDest);

                            if (ehIsBlockEHLast(bNext))
                            {
                                ehUpdateLastBlocks(bNext, bDest);
                            }
                        }
                    }

                    if (optimizeJump)
                    {
                        JITDUMP("\nReversing a conditional jump around an unconditional jump (" FMT_BB " -> " FMT_BB
                                ", " FMT_BB " -> " FMT_BB ")\n",
                                block->bbNum, bDest->bbNum, bNext->bbNum, bNextJumpDest->bbNum);

                        //  Reverse the jump condition
                        //
                        GenTree* test = block->lastNode();
                        noway_assert(test->OperIsConditionalJump());

                        if (test->OperIs(GT_JTRUE))
                        {
                            GenTree* cond = gtReverseCond(test->AsOp()->gtOp1);
                            assert(cond == test->AsOp()->gtOp1); // Ensure `gtReverseCond` did not create a new node.
                            test->AsOp()->gtOp1 = cond;
                        }
                        else
                        {
                            gtReverseCond(test);
                        }

                        // Rewire flow from block
                        //
                        std::swap(block->TrueEdgeRef(), block->FalseEdgeRef());
                        fgRedirectEdge(block->TrueEdgeRef(), bNext->GetTarget());

                        // bNext no longer flows to target
                        //
                        fgRemoveRefPred(bNext->GetTargetEdge());

                        /*
                          Unlink bNext from the BasicBlock list; note that we can
                          do this even though other blocks could jump to it - the
                          reason is that elsewhere in this function we always
                          redirect jumps to jumps to jump to the final label,
                          so even if another block jumps to bNext it won't matter
                          once we're done since any such jump will be redirected
                          to the final target by the time we're done here.
                        */

                        fgUnlinkBlockForRemoval(bNext);

                        /* Mark the block as removed */
                        bNext->SetFlags(BBF_REMOVED);

                        //
                        // If we removed the end of a try region or handler region
                        // we will need to update ebdTryLast or ebdHndLast.
                        //

                        for (EHblkDsc* const HBtab : EHClauses(this))
                        {
                            if ((HBtab->ebdTryLast == bNext) || (HBtab->ebdHndLast == bNext))
                            {
                                fgSkipRmvdBlocks(HBtab);
                            }
                        }

                        // we optimized this JUMP - goto REPEAT to catch similar cases
                        change   = true;
                        modified = true;

#ifdef DEBUG
                        if (verbose)
                        {
                            printf("\nAfter reversing the jump:\n");
                            fgDispBasicBlocks(verboseTrees);
                        }
#endif // DEBUG

                        /*
                           For a rare special case we cannot jump to REPEAT
                           as jumping to REPEAT will cause us to delete 'block'
                           because it currently appears to be unreachable.  As
                           it is a self loop that only has a single bbRef (itself)
                           However since the unlinked bNext has additional bbRefs
                           (that we will later connect to 'block'), it is not really
                           unreachable.
                        */
                        if ((bNext->bbRefs > 0) && bNext->TargetIs(block) && (block->bbRefs == 1))
                        {
                            continue;
                        }

                        goto REPEAT;
                    }
                }
            }

            //
            // Update the switch jump table such that it follows jumps to jumps:
            //
            if (block->KindIs(BBJ_SWITCH))
            {
                if (fgOptimizeSwitchBranches(block))
                {
                    change   = true;
                    modified = true;
                    goto REPEAT;
                }
            }

            noway_assert(!block->HasFlag(BBF_REMOVED));

            /* COMPACT blocks if possible */

            if (fgCanCompactBlock(block))
            {
                fgCompactBlock(block);

                /* we compacted two blocks - goto REPEAT to catch similar cases */
                change   = true;
                modified = true;
                bPrev    = block->Prev();
                goto REPEAT;
            }

            // Remove unreachable or empty blocks - do not consider blocks marked BBF_DONT_REMOVE
            // These include first and last block of a TRY, exception handlers and THROW blocks.
            if (block->HasFlag(BBF_DONT_REMOVE))
            {
                bPrev = block;
                continue;
            }

            assert(!bbIsTryBeg(block));
            noway_assert(block->bbCatchTyp == BBCT_NONE);

            /* Remove unreachable blocks
             *
             * We'll look for blocks that have countOfInEdges() = 0 (blocks may become
             * unreachable due to a BBJ_ALWAYS introduced by conditional folding for example)
             */

            if (block->countOfInEdges() == 0)
            {
                /* no references -> unreachable - remove it */
                /* For now do not update the bbNum, do it at the end */

                fgRemoveBlock(block, /* unreachable */ true);

                change   = true;
                modified = true;

                /* we removed the current block - the rest of the optimizations won't have a target
                 * continue with the next one */

                continue;
            }
            else if (block->countOfInEdges() == 1)
            {
                switch (block->GetKind())
                {
                    case BBJ_COND:
                        if (block->TrueTargetIs(block) || block->FalseTargetIs(block))
                        {
                            fgRemoveBlock(block, /* unreachable */ true);

                            change   = true;
                            modified = true;

                            /* we removed the current block - the rest of the optimizations
                             * won't have a target so continue with the next block */

                            continue;
                        }
                        break;
                    case BBJ_ALWAYS:
                        if (block->TargetIs(block))
                        {
                            fgRemoveBlock(block, /* unreachable */ true);

                            change   = true;
                            modified = true;

                            /* we removed the current block - the rest of the optimizations
                             * won't have a target so continue with the next block */

                            continue;
                        }
                        break;

                    default:
                        break;
                }
            }

            noway_assert(!block->HasFlag(BBF_REMOVED));

            /* Remove EMPTY blocks */

            if (block->isEmpty())
            {
                assert(block->PrevIs(bPrev));
                if (fgOptimizeEmptyBlock(block))
                {
                    change   = true;
                    modified = true;
                }

                /* Have we removed the block? */

                if (block->HasFlag(BBF_REMOVED))
                {
                    /* block was removed - no change to bPrev */
                    continue;
                }
            }

            /* Set the predecessor of the last reachable block
             * If we removed the current block, the predecessor remains unchanged
             * otherwise, since the current block is ok, it becomes the predecessor */

            noway_assert(!block->HasFlag(BBF_REMOVED));

            bPrev = block;
        }
    } while (change);

    // OSR entry blocks will frequently have a profile imbalance as original method execution was hijacked at them.
    // Mark the profile as inconsistent if we might have propagated the OSR entry weight.
    if (modified && opts.IsOSR())
    {
        JITDUMP("fgUpdateFlowGraph: Inconsistent OSR entry weight may have been propagated. Data %s consistent.\n",
                fgPgoConsistent ? "is now" : "was already");
        fgPgoConsistent = false;
    }

#ifdef DEBUG
    if (!isPhase)
    {
        if (verbose && modified)
        {
            printf("\nAfter updating the flow graph:\n");
            fgDispBasicBlocks(verboseTrees);
            fgDispHandlerTab();
        }

        if (compRationalIRForm)
        {
            for (BasicBlock* const block : Blocks())
            {
                LIR::AsRange(block).CheckLIR(this);
            }
        }

        fgVerifyHandlerTab();
        // Make sure that the predecessor lists are accurate
        fgDebugCheckBBlist();
        fgDebugCheckUpdate();
    }
#endif // DEBUG

    return modified;
}

//-------------------------------------------------------------
// fgDfsBlocksAndRemove: Compute DFS and delete dead blocks.
//
// Returns:
//    Suitable phase status
//
PhaseStatus Compiler::fgDfsBlocksAndRemove()
{
    fgInvalidateDfsTree();
    m_dfsTree = fgComputeDfs();

    return fgRemoveBlocksOutsideDfsTree() ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//-------------------------------------------------------------
// fgRemoveBlocksOutsideDfsTree: Remove the blocks that are not in the current DFS tree.
//
// Returns:
//    True if any block was removed.
//
bool Compiler::fgRemoveBlocksOutsideDfsTree()
{
    if (m_dfsTree->GetPostOrderCount() == fgBBcount)
    {
        return false;
    }

#ifdef DEBUG
    if (verbose)
    {
        printf("%u/%u blocks are unreachable and will be removed:\n", fgBBcount - m_dfsTree->GetPostOrderCount(),
               fgBBcount);
        for (BasicBlock* block : Blocks())
        {
            if (!m_dfsTree->Contains(block))
            {
                printf("  " FMT_BB "\n", block->bbNum);
            }
        }
    }
#endif // DEBUG

    // The DFS we run is not precise around call-finally, so
    // `fgRemoveUnreachableBlocks` can expose newly unreachable blocks
    // that we did not uncover during the DFS. If we did remove any
    // call-finally blocks then iterate to closure. This is a very rare
    // case.
    while (true)
    {
        bool anyCallFinallyPairs = false;
        fgRemoveUnreachableBlocks([=, &anyCallFinallyPairs](BasicBlock* block) {
            if (!m_dfsTree->Contains(block))
            {
                anyCallFinallyPairs |= block->isBBCallFinallyPair();
                return true;
            }

            return false;
        });

        if (!anyCallFinallyPairs)
        {
            break;
        }

        m_dfsTree = fgComputeDfs();
    }

#ifdef DEBUG
    // Did we actually remove all the blocks we said we were going to?
    if (verbose)
    {
        if (m_dfsTree->GetPostOrderCount() != fgBBcount)
        {
            printf("%u unreachable blocks were not removed:\n", fgBBcount - m_dfsTree->GetPostOrderCount());
            for (BasicBlock* block : Blocks())
            {
                if (!m_dfsTree->Contains(block))
                {
                    printf("  " FMT_BB "\n", block->bbNum);
                }
            }
        }
    }
#endif // DEBUG

    return true;
}

//-------------------------------------------------------------
// fgGetCodeEstimate: Compute a code size estimate for the block, including all statements
// and block control flow.
//
// Arguments:
//    block - block to consider
//
// Returns:
//    Code size estimate for block
//
unsigned Compiler::fgGetCodeEstimate(BasicBlock* block)
{
    unsigned costSz = 0; // estimate of block's code size cost

    switch (block->GetKind())
    {
        case BBJ_ALWAYS:
        case BBJ_EHCATCHRET:
        case BBJ_LEAVE:
        case BBJ_COND:
            costSz = 2;
            break;
        case BBJ_CALLFINALLY:
            costSz = 5;
            break;
        case BBJ_CALLFINALLYRET:
            costSz = 0;
            break;
        case BBJ_SWITCH:
            costSz = 10;
            break;
        case BBJ_THROW:
            costSz = 1; // We place a int3 after the code for a throw block
            break;
        case BBJ_EHFINALLYRET:
        case BBJ_EHFAULTRET:
        case BBJ_EHFILTERRET:
            costSz = 1;
            break;
        case BBJ_RETURN: // return from method
            costSz = 3;
            break;
        default:
            noway_assert(!"Bad bbKind");
            break;
    }

    for (Statement* const stmt : block->NonPhiStatements())
    {
        unsigned char cost = stmt->GetCostSz();
        costSz += cost;
    }

    return costSz;
}

#ifdef FEATURE_JIT_METHOD_PERF

//------------------------------------------------------------------------
// fgMeasureIR: count and return the number of IR nodes in the function.
//
unsigned Compiler::fgMeasureIR()
{
    unsigned nodeCount = 0;

    for (BasicBlock* const block : Blocks())
    {
        if (!block->IsLIR())
        {
            for (Statement* const stmt : block->Statements())
            {
                fgWalkTreePre(
                    stmt->GetRootNodePointer(),
                    [](GenTree** slot, fgWalkData* data) -> Compiler::fgWalkResult {
                    (*reinterpret_cast<unsigned*>(data->pCallbackData))++;
                    return Compiler::WALK_CONTINUE;
                },
                    &nodeCount);
            }
        }
        else
        {
            for (GenTree* node : LIR::AsRange(block))
            {
                nodeCount++;
            }
        }
    }

    return nodeCount;
}

#endif // FEATURE_JIT_METHOD_PERF

//------------------------------------------------------------------------
// fgHeadTailMerge: merge common sequences of statements in block predecessors/successors
//
// Parameters:
//   early - Whether this is being checked with early IR invariants (where
//           we do not have valid address exposure/GTF_GLOB_REF).
//
// Returns:
//   Suitable phase status.
//
// Notes:
//   This applies tail merging and head merging. For tail merging it looks for
//   cases where all or some predecessors of a block have the same (or
//   equivalent) last statement.
//
//   If all predecessors have the same last statement, move one of them to
//   the start of the block, and delete the copies in the preds.
//   Then retry merging.
//
//   If some predecessors have the same last statement, pick one as the
//   canonical, split it if necessary, cross jump from the others to
//   the canonical, and delete the copies in the cross jump blocks.
//   Then retry merging on the canonical block.
//
//   Conversely, for head merging, we look for cases where all successors of a
//   block start with the same statement. We then try to move one of them into
//   the predecessor (which requires special handling due to the terminator
//   node) and delete the copies.
//
//   We set a mergeLimit to try and get most of the benefit while not
//   incurring too much TP overhead. It's possible to make the merging
//   more efficient and if so it might be worth revising this value.
//
PhaseStatus Compiler::fgHeadTailMerge(bool early)
{
    bool      madeChanges = false;
    int const mergeLimit  = 50;

    const bool isEnabled = JitConfig.JitEnableHeadTailMerge() > 0;
    if (!isEnabled)
    {
        JITDUMP("Head and tail merge disabled by JitEnableHeadTailMerge\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }

#ifdef DEBUG
    static ConfigMethodRange JitEnableHeadTailMergeRange;
    JitEnableHeadTailMergeRange.EnsureInit(JitConfig.JitEnableHeadTailMergeRange());
    const unsigned hash = impInlineRoot()->info.compMethodHash();
    if (!JitEnableHeadTailMergeRange.Contains(hash))
    {
        JITDUMP("Tail merge disabled by JitEnableHeadTailMergeRange\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }
#endif

    struct PredInfo
    {
        PredInfo(BasicBlock* block, Statement* stmt)
            : m_block(block)
            , m_stmt(stmt)
        {
        }
        BasicBlock* m_block;
        Statement*  m_stmt;
    };

    ArrayStack<PredInfo>    predInfo(getAllocator(CMK_ArrayStack));
    ArrayStack<PredInfo>    matchedPredInfo(getAllocator(CMK_ArrayStack));
    ArrayStack<BasicBlock*> retryBlocks(getAllocator(CMK_ArrayStack));

    // Try tail merging a block.
    // If return value is true, retry.
    // May also add to retryBlocks.
    //
    auto tailMergePreds = [&](BasicBlock* commSucc) -> bool {
        // Are there enough preds to make it interesting?
        //
        if (predInfo.Height() < 2)
        {
            // Not enough preds to merge
            return false;
        }

        // If there are large numbers of viable preds, forgo trying to merge.
        // While there can be large benefits, there can also be large costs.
        //
        // Note we check this rather than countOfInEdges because we don't care
        // about dups, just the number of unique pred blocks.
        //
        if (predInfo.Height() > mergeLimit)
        {
            // Too many preds to consider
            return false;
        }

        // Find a matching set of preds. Potentially O(N^2) tree comparisons.
        //
        int i = 0;
        while (i < (predInfo.Height() - 1))
        {
            matchedPredInfo.Reset();
            matchedPredInfo.Emplace(predInfo.TopRef(i));
            Statement* const  baseStmt  = predInfo.TopRef(i).m_stmt;
            BasicBlock* const baseBlock = predInfo.TopRef(i).m_block;

            for (int j = i + 1; j < predInfo.Height(); j++)
            {
                BasicBlock* const otherBlock = predInfo.TopRef(j).m_block;

                // Consider: bypass this for statements that can't cause exceptions.
                //
                if (!BasicBlock::sameEHRegion(baseBlock, otherBlock))
                {
                    continue;
                }

                Statement* const otherStmt = predInfo.TopRef(j).m_stmt;

                // Consider: compute and cache hashes to make this faster
                //
                if (GenTree::Compare(baseStmt->GetRootNode(), otherStmt->GetRootNode()))
                {
                    matchedPredInfo.Emplace(predInfo.TopRef(j));
                }
            }

            if (matchedPredInfo.Height() < 2)
            {
                // This pred didn't match any other. Check other preds for matches.
                i++;
                continue;
            }

            // We can move the identical last statements to commSucc, if it exists,
            // and all preds have matching last statements, and we're not changing EH behavior.
            //
            bool const hasCommSucc               = (commSucc != nullptr);
            bool const predsInSameEHRegionAsSucc = hasCommSucc && BasicBlock::sameEHRegion(baseBlock, commSucc);
            bool const canMergeAllPreds = hasCommSucc && (matchedPredInfo.Height() == (int)commSucc->countOfInEdges());
            bool const canMergeIntoSucc = predsInSameEHRegionAsSucc && canMergeAllPreds;

            if (canMergeIntoSucc)
            {
                JITDUMP("All %d preds of " FMT_BB " end with the same tree, moving\n", matchedPredInfo.Height(),
                        commSucc->bbNum);
                JITDUMPEXEC(gtDispStmt(matchedPredInfo.TopRef(0).m_stmt));

                for (int j = 0; j < matchedPredInfo.Height(); j++)
                {
                    PredInfo&         info      = matchedPredInfo.TopRef(j);
                    Statement* const  stmt      = info.m_stmt;
                    BasicBlock* const predBlock = info.m_block;

                    fgUnlinkStmt(predBlock, stmt);

                    // Add one of the matching stmts to block, and
                    // update its flags.
                    //
                    if (j == 0)
                    {
                        fgInsertStmtAtBeg(commSucc, stmt);
                        commSucc->CopyFlags(predBlock, BBF_COPY_PROPAGATE);
                    }

                    madeChanges = true;
                }

                // It's worth retrying tail merge on this block.
                //
                return true;
            }

            // All or a subset of preds have matching last stmt, we will cross-jump.
            // Pick one pred block as the victim -- preferably a block with just one
            // statement or one that falls through to block (or both).
            //
            if (predsInSameEHRegionAsSucc)
            {
                JITDUMP("A subset of %d preds of " FMT_BB " end with the same tree\n", matchedPredInfo.Height(),
                        commSucc->bbNum);
            }
            else if (commSucc != nullptr)
            {
                JITDUMP("%s %d preds of " FMT_BB " end with the same tree but are in a different EH region\n",
                        canMergeAllPreds ? "All" : "A subset of", matchedPredInfo.Height(), commSucc->bbNum);
            }
            else
            {
                JITDUMP("A set of %d return blocks end with the same tree\n", matchedPredInfo.Height());
            }

            JITDUMPEXEC(gtDispStmt(matchedPredInfo.TopRef(0).m_stmt));

            BasicBlock* crossJumpVictim       = nullptr;
            Statement*  crossJumpStmt         = nullptr;
            bool        haveNoSplitVictim     = false;
            bool        haveFallThroughVictim = false;

            for (int j = 0; j < matchedPredInfo.Height(); j++)
            {
                PredInfo&         info      = matchedPredInfo.TopRef(j);
                Statement* const  stmt      = info.m_stmt;
                BasicBlock* const predBlock = info.m_block;

                // Never pick the init block as the victim as that would
                // cause us to add a predecessor to it, which is invalid.
                if (predBlock == fgFirstBB)
                {
                    continue;
                }

                bool const isNoSplit     = stmt == predBlock->firstStmt();
                bool const isFallThrough = (predBlock->KindIs(BBJ_ALWAYS) && predBlock->JumpsToNext());

                // Is this block possibly better than what we have?
                //
                bool useBlock = false;

                if (crossJumpVictim == nullptr)
                {
                    // Pick an initial candidate.
                    useBlock = true;
                }
                else if (isNoSplit && isFallThrough)
                {
                    // This is the ideal choice.
                    //
                    useBlock = true;
                }
                else if (!haveNoSplitVictim && isNoSplit)
                {
                    useBlock = true;
                }
                else if (!haveNoSplitVictim && !haveFallThroughVictim && isFallThrough)
                {
                    useBlock = true;
                }

                if (useBlock)
                {
                    crossJumpVictim       = predBlock;
                    crossJumpStmt         = stmt;
                    haveNoSplitVictim     = isNoSplit;
                    haveFallThroughVictim = isFallThrough;
                }

                // If we have the perfect victim, stop looking.
                //
                if (haveNoSplitVictim && haveFallThroughVictim)
                {
                    break;
                }
            }

            BasicBlock* crossJumpTarget = crossJumpVictim;

            // If this block requires splitting, then split it.
            // Note we know that stmt has a prev stmt.
            //
            if (haveNoSplitVictim)
            {
                JITDUMP("Will cross-jump to " FMT_BB "\n", crossJumpTarget->bbNum);
            }
            else
            {
                crossJumpTarget = fgSplitBlockAfterStatement(crossJumpVictim, crossJumpStmt->GetPrevStmt());
                JITDUMP("Will cross-jump to newly split off " FMT_BB "\n", crossJumpTarget->bbNum);
            }

            assert(!crossJumpTarget->isEmpty());

            // Do the cross jumping
            //
            for (int j = 0; j < matchedPredInfo.Height(); j++)
            {
                PredInfo&         info      = matchedPredInfo.TopRef(j);
                BasicBlock* const predBlock = info.m_block;
                Statement* const  stmt      = info.m_stmt;

                if (predBlock == crossJumpVictim)
                {
                    continue;
                }

                // remove the statement
                fgUnlinkStmt(predBlock, stmt);

                // Fix up the flow.
                //
                if (commSucc != nullptr)
                {
                    assert(predBlock->KindIs(BBJ_ALWAYS));
                    fgRedirectEdge(predBlock->TargetEdgeRef(), crossJumpTarget);
                }
                else
                {
                    FlowEdge* const newEdge = fgAddRefPred(crossJumpTarget, predBlock);
                    predBlock->SetKindAndTargetEdge(BBJ_ALWAYS, newEdge);
                }

                // For tail merge we have a common successor of predBlock and
                // crossJumpTarget, so the profile update can be done locally.
                if (crossJumpTarget->hasProfileWeight())
                {
                    crossJumpTarget->increaseBBProfileWeight(predBlock->bbWeight);
                }
            }

            // We changed things
            //
            madeChanges = true;

            // We should try tail merging the cross jump target.
            //
            retryBlocks.Push(crossJumpTarget);

            // Continue trying to merge in the current block.
            // This is a bit inefficient, we could remember how
            // far we got through the pred list perhaps.
            //
            return true;
        }

        // We've looked at everything.
        //
        return false;
    };

    auto tailMerge = [&](BasicBlock* block) -> bool {
        if (block->countOfInEdges() < 2)
        {
            // Nothing to merge here
            return false;
        }

        predInfo.Reset();

        // Find the subset of preds that reach along non-critical edges
        // and populate predInfo.
        //
        for (BasicBlock* const predBlock : block->PredBlocks())
        {
            if (predBlock->GetUniqueSucc() != block)
            {
                continue;
            }

            Statement* lastStmt = predBlock->lastStmt();

            // Block might be empty.
            //
            if (lastStmt == nullptr)
            {
                continue;
            }

            // Walk back past any GT_NOPs.
            //
            Statement* const firstStmt = predBlock->firstStmt();
            while (lastStmt->GetRootNode()->OperIs(GT_NOP))
            {
                if (lastStmt == firstStmt)
                {
                    // predBlock is evidently all GT_NOP.
                    //
                    lastStmt = nullptr;
                    break;
                }

                lastStmt = lastStmt->GetPrevStmt();
            }

            // Block might be effectively empty.
            //
            if (lastStmt == nullptr)
            {
                continue;
            }

            // We don't expect to see PHIs but watch for them anyways.
            //
            assert(!lastStmt->IsPhiDefnStmt());
            predInfo.Emplace(predBlock, lastStmt);
        }

        return tailMergePreds(block);
    };

    auto iterateTailMerge = [&](BasicBlock* block) -> void {
        int numOpts = 0;

        while (tailMerge(block))
        {
            numOpts++;
        }

        if (numOpts > 0)
        {
            JITDUMP("Did %d tail merges in " FMT_BB "\n", numOpts, block->bbNum);
        }
    };

    ArrayStack<BasicBlock*> retBlocks(getAllocator(CMK_ArrayStack));

    // Visit each block
    //
    for (BasicBlock* const block : Blocks())
    {
        iterateTailMerge(block);

        if (block->KindIs(BBJ_RETURN) && !block->isEmpty() && (block != genReturnBB))
        {
            // Avoid spitting a return away from a possible tail call
            //
            if (!block->hasSingleStmt())
            {
                Statement* const lastStmt = block->lastStmt();
                Statement* const prevStmt = lastStmt->GetPrevStmt();
                GenTree* const   prevTree = prevStmt->GetRootNode();
                if (prevTree->IsCall() && prevTree->AsCall()->CanTailCall())
                {
                    continue;
                }
            }

            retBlocks.Push(block);
        }
    }

    predInfo.Reset();
    for (int i = 0; i < retBlocks.Height(); i++)
    {
        predInfo.Push(PredInfo(retBlocks.Bottom(i), retBlocks.Bottom(i)->lastStmt()));
    }

    tailMergePreds(nullptr);

    // Work through any retries
    //
    while (retryBlocks.Height() > 0)
    {
        iterateTailMerge(retryBlocks.Pop());
    }

    // Visit each block and try to merge first statements of successors.
    //
    for (BasicBlock* const block : Blocks())
    {
        madeChanges |= fgHeadMerge(block, early);
    }

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//------------------------------------------------------------------------
// fgTryOneHeadMerge: Try to merge the first statement of the successors of a
// specified block.
//
// Parameters:
//   block - The block whose successors are to be considered
//   early - Whether this is being checked with early IR invariants
//           (where we do not have valid address exposure/GTF_GLOB_REF).
//
// Returns:
//   True if the merge succeeded.
//
bool Compiler::fgTryOneHeadMerge(BasicBlock* block, bool early)
{
    // We currently only check for BBJ_COND, which gets the common case of
    // spill clique created stores by the importer (often produced due to
    // ternaries in C#).
    // The logic below could be generalized to BBJ_SWITCH, but this currently
    // has almost no CQ benefit but does have a TP impact.
    if (!block->KindIs(BBJ_COND) || block->TrueEdgeIs(block->GetFalseEdge()))
    {
        return false;
    }

    // Verify that both successors are reached along non-critical edges.
    auto getSuccCandidate = [=](BasicBlock* succ, Statement** firstStmt) -> bool {
        if (succ->GetUniquePred(this) != block)
        {
            return false;
        }

        if (!BasicBlock::sameEHRegion(block, succ))
        {
            return false;
        }

        *firstStmt = nullptr;
        // Walk past any GT_NOPs.
        //
        for (Statement* stmt : succ->Statements())
        {
            if (!stmt->GetRootNode()->OperIs(GT_NOP))
            {
                *firstStmt = stmt;
                break;
            }
        }

        // Block might be effectively empty.
        //
        if (*firstStmt == nullptr)
        {
            return false;
        }

        // Cannot move terminator statement.
        //
        if ((*firstStmt == succ->lastStmt()) && succ->HasTerminator())
        {
            return false;
        }

        return true;
    };

    Statement* nextFirstStmt;
    Statement* destFirstStmt;

    if (!getSuccCandidate(block->GetFalseTarget(), &nextFirstStmt) ||
        !getSuccCandidate(block->GetTrueTarget(), &destFirstStmt))
    {
        return false;
    }

    if (!GenTree::Compare(nextFirstStmt->GetRootNode(), destFirstStmt->GetRootNode()))
    {
        return false;
    }

    JITDUMP("Both succs of " FMT_BB " start with the same tree\n", block->bbNum);
    DISPSTMT(nextFirstStmt);

    if (gtTreeContainsTailCall(nextFirstStmt->GetRootNode()) || gtTreeContainsTailCall(destFirstStmt->GetRootNode()))
    {
        JITDUMP("But one is a tailcall\n");
        return false;
    }

    JITDUMP("Checking if we can move it into the predecessor...\n");

    if (!fgCanMoveFirstStatementIntoPred(early, nextFirstStmt, block))
    {
        return false;
    }

    JITDUMP("We can; moving statement\n");

    fgUnlinkStmt(block->GetFalseTarget(), nextFirstStmt);
    fgInsertStmtNearEnd(block, nextFirstStmt);
    fgUnlinkStmt(block->GetTrueTarget(), destFirstStmt);
    block->CopyFlags(block->GetFalseTarget(), BBF_COPY_PROPAGATE);

    return true;
}

//------------------------------------------------------------------------
// fgHeadMerge: Try to repeatedly merge the first statement of the successors
// of the specified block.
//
// Parameters:
//   block               - The block whose successors are to be considered
//   early               - Whether this is being checked with early IR invariants
//                         (where we do not have valid address exposure/GTF_GLOB_REF).
//
// Returns:
//   True if any merge succeeded.
//
bool Compiler::fgHeadMerge(BasicBlock* block, bool early)
{
    bool madeChanges = false;
    int  numOpts     = 0;
    while (fgTryOneHeadMerge(block, early))
    {
        madeChanges = true;
        numOpts++;
    }

    if (numOpts > 0)
    {
        JITDUMP("Did %d head merges in " FMT_BB "\n", numOpts, block->bbNum);
    }

    return madeChanges;
}

//------------------------------------------------------------------------
// gtTreeContainsTailCall: Check if a tree contains any tail call or tail call
// candidate.
//
// Parameters:
//   tree - The tree
//
// Remarks:
//   While tail calls are generally expected to be top level nodes we do allow
//   some other shapes of calls to be tail calls, including some cascading
//   trivial assignments and casts. This function does a tree walk to check if
//   any sub tree is a tail call.
//
bool Compiler::gtTreeContainsTailCall(GenTree* tree)
{
    auto isTailCall = [](GenTree* tree) {
        return tree->IsCall() && (tree->AsCall()->CanTailCall() || tree->AsCall()->IsTailCall());
    };

    return gtFindNodeInTree<GTF_CALL>(tree, isTailCall) != nullptr;
}

//------------------------------------------------------------------------
// gtTreeContainsAsyncCall: Check if a tree contains any async call.
//
// Parameters:
//   tree - The tree to check
//
// Returns:
//   True if any node in the tree is an async call, false otherwise.
//
bool Compiler::gtTreeContainsAsyncCall(GenTree* tree)
{
    if (!compIsAsync())
    {
        return false;
    }

    auto isAsyncCall = [](GenTree* tree) {
        return tree->IsCall() && tree->AsCall()->IsAsync();
    };

    return gtFindNodeInTree<GTF_CALL>(tree, isAsyncCall) != nullptr;
}

//------------------------------------------------------------------------
// fgCanMoveFirstStatementIntoPred: Check if the first statement of a block can
// be moved into its predecessor.
//
// Parameters:
//   early     - Whether this is being checked with early IR invariants (where
//               we do not have valid address exposure/GTF_GLOB_REF).
//   firstStmt - The statement to move
//   pred      - The predecessor block
//
// Remarks:
//   Unlike tail merging, for head merging we have to either spill the
//   predecessor's terminator node, or reorder it with the head statement.
//   Here we choose to reorder.
//
bool Compiler::fgCanMoveFirstStatementIntoPred(bool early, Statement* firstStmt, BasicBlock* pred)
{
    if (!pred->HasTerminator())
    {
        return true;
    }

    GenTree* tree1 = pred->lastStmt()->GetRootNode();
    GenTree* tree2 = firstStmt->GetRootNode();

    GenTreeFlags tree1Flags = tree1->gtFlags;
    GenTreeFlags tree2Flags = tree2->gtFlags;

    if (early)
    {
        tree1Flags |= gtHasLocalsWithAddrOp(tree1) ? GTF_GLOB_REF : GTF_EMPTY;
        tree2Flags |= gtHasLocalsWithAddrOp(tree2) ? GTF_GLOB_REF : GTF_EMPTY;
    }

    // We do not support embedded statements in the terminator node.
    if ((tree1Flags & GTF_ASG) != 0)
    {
        JITDUMP("  no; terminator contains embedded store\n");
        return false;
    }
    if ((tree2Flags & GTF_ASG) != 0)
    {
        // Handle common case where the second statement is a top-level store.
        if (!tree2->OperIsLocalStore())
        {
            JITDUMP("  cannot reorder with GTF_ASG without top-level store");
            return false;
        }

        GenTreeLclVarCommon* lcl = tree2->AsLclVarCommon();
        if ((lcl->Data()->gtFlags & GTF_ASG) != 0)
        {
            JITDUMP("  cannot reorder with embedded store");
            return false;
        }

        LclVarDsc* dsc = lvaGetDesc(tree2->AsLclVarCommon());
        if ((tree1Flags & GTF_ALL_EFFECT) != 0)
        {
            if (early ? dsc->lvHasLdAddrOp : dsc->IsAddressExposed())
            {
                JITDUMP("  cannot reorder store to exposed local with any side effect\n");
                return false;
            }

            if (((tree1Flags & (GTF_CALL | GTF_EXCEPT)) != 0) && pred->HasPotentialEHSuccs(this))
            {
                JITDUMP("  cannot reorder store with exception throwing tree and potential EH successor\n");
                return false;
            }
        }

        if (gtHasRef(tree1, lcl->GetLclNum()))
        {
            JITDUMP("  cannot reorder with interfering use\n");
            return false;
        }

        if (dsc->lvIsStructField && gtHasRef(tree1, dsc->lvParentLcl))
        {
            JITDUMP("  cannot reorder with interfering use of parent struct local\n");
            return false;
        }

        if (dsc->lvPromoted)
        {
            for (int i = 0; i < dsc->lvFieldCnt; i++)
            {
                if (gtHasRef(tree1, dsc->lvFieldLclStart + i))
                {
                    JITDUMP("  cannot reorder with interfering use of struct field\n");
                    return false;
                }
            }
        }

        // We've validated that the store does not interfere. Get rid of the
        // flag for the future checks.
        tree2Flags &= ~GTF_ASG;
    }

    if (((tree1Flags & GTF_CALL) != 0) && ((tree2Flags & GTF_ALL_EFFECT) != 0))
    {
        JITDUMP("  cannot reorder call with any side effect\n");
        return false;
    }
    if (((tree1Flags & GTF_GLOB_REF) != 0) && ((tree2Flags & GTF_PERSISTENT_SIDE_EFFECTS) != 0))
    {
        JITDUMP("  cannot reorder global reference with persistent side effects\n");
        return false;
    }
    if ((tree1Flags & GTF_ORDER_SIDEEFF) != 0)
    {
        if ((tree2Flags & (GTF_GLOB_REF | GTF_ORDER_SIDEEFF)) != 0)
        {
            JITDUMP("  cannot reorder ordering side effect\n");
            return false;
        }
    }
    if ((tree2Flags & GTF_ORDER_SIDEEFF) != 0)
    {
        if ((tree1Flags & (GTF_GLOB_REF | GTF_ORDER_SIDEEFF)) != 0)
        {
            JITDUMP("  cannot reorder ordering side effect\n");
            return false;
        }
    }
    if (((tree1Flags & GTF_EXCEPT) != 0) && ((tree2Flags & GTF_SIDE_EFFECT) != 0))
    {
        JITDUMP("  cannot reorder exception with side effect\n");
        return false;
    }

    return true;
}

//-------------------------------------------------------------
// fgResolveGDVs: Try and resolve GDV checks
//
// Returns:
//    Suitable phase status.
//
PhaseStatus Compiler::fgResolveGDVs()
{
    if (!opts.OptimizationEnabled())
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    if (!doesMethodHaveGuardedDevirtualization())
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    if (!hasUpdatedTypeLocals)
    {
        return PhaseStatus::MODIFIED_NOTHING;
    }

    bool madeChanges = false;

    for (BasicBlock* const block : Blocks())
    {
        if (!block->KindIs(BBJ_COND))
        {
            continue;
        }

        GuardInfo info;
        if (ObjectAllocator::IsGuard(block, &info))
        {
            assert(block == info.m_block);
            LclVarDsc* const lclDsc = lvaGetDesc(info.m_local);

            if (lclDsc->lvClassIsExact && lclDsc->lvSingleDef && (lclDsc->lvClassHnd == info.m_type))
            {
                JITDUMP("GDV in " FMT_BB " can be resolved; type is now known exactly\n", block->bbNum);

                bool const      isCondTrue   = info.m_relop->OperIs(GT_EQ);
                FlowEdge* const retainedEdge = isCondTrue ? block->GetTrueEdge() : block->GetFalseEdge();
                FlowEdge* const removedEdge  = isCondTrue ? block->GetFalseEdge() : block->GetTrueEdge();

                JITDUMP("The conditional jump becomes an unconditional jump to " FMT_BB "\n",
                        retainedEdge->getDestinationBlock()->bbNum);

                fgRemoveRefPred(removedEdge);
                block->SetKindAndTargetEdge(BBJ_ALWAYS, retainedEdge);
                fgRepairProfileCondToUncond(block, retainedEdge, removedEdge);

                // The GDV relop will typically be side effecting so just
                // leave it in place for later cleanup.
                //
                info.m_stmt->SetRootNode(info.m_relop);
                madeChanges = true;
            }
        }
    }

    return madeChanges ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}
