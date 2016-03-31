//===------------------------------------------------------------*- C++ -*-===//
//
// This file is distributed under BSD License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Copyright (c) 2016 University of Kaiserslautern.

#include "SectionDisassemblyAnalyzerARM.h"
#include "disasm/SectionDisassemblyARM.h"
#include <iostream>
#include <algorithm>
#include <cassert>

namespace disasm {

SectionDisassemblyAnalyzerARM::SectionDisassemblyAnalyzerARM
    (SectionDisassemblyARM *sec_disasm,
     const std::pair<addr_t, addr_t> &exec_region) :
    m_sec_disassembly{sec_disasm},
    m_analyzer{sec_disasm->getISA()},
    m_exec_addr_start{exec_region.first},
    m_exec_addr_end{exec_region.second} {
}

size_t SectionDisassemblyAnalyzerARM::calculateBasicBlockWeight
    (const CFGNode &node, const BasicBlock &basic_block) const noexcept {
    unsigned pred_weight = 0;
    for (auto pred_iter = node.getDirectPredecessors().cbegin();
         pred_iter < node.getDirectPredecessors().cend(); ++pred_iter) {
        if (!(*pred_iter).node()->isData()) {
            pred_weight +=
                (*pred_iter).node()->maximalBlock()->instructionsCount();
        }
    }
    return pred_weight + basic_block.instructionCount();
}

size_t SectionDisassemblyAnalyzerARM::calculateNodeWeight
    (const CFGNode *node) const noexcept {
    if (node->isData()) {
        return 0;
    }
    unsigned pred_weight = 0;
    for (auto pred_iter = node->getDirectPredecessors().cbegin();
         pred_iter < node->getDirectPredecessors().cend(); ++pred_iter) {
        pred_weight +=
            (*pred_iter).node()->maximalBlock()->instructionsCount();
    }
    return node->maximalBlock()->instructionsCount() + pred_weight;
}

void SectionDisassemblyAnalyzerARM::buildCFG() {
    if (m_sec_disassembly->maximalBlockCount() == 0) {
        return;
    }
    // work directly with the vector of CFGNode
    auto &cfg = m_sec_cfg.m_cfg;
    cfg.resize(m_sec_disassembly->maximalBlockCount());
    {
        MaximalBlock *first_maximal_block =
            &(*m_sec_disassembly->getMaximalBlocks().begin());
        // handle first MB
        cfg.front().setMaximalBlock(first_maximal_block);
        if (first_maximal_block->getBranch().isDirect()
            && !isValidCodeAddr(first_maximal_block->getBranch().target())) {
            // a branch to an address outside of executable code
            cfg.front().setToDataAndInvalidatePredecessors();
        }
    }
    {
        // first pass over MBs to mark overlap and invalid targets skipping first MB
        auto node_iter = cfg.begin() + 1;
        for (auto block_iter =
            m_sec_disassembly->getMaximalBlocks().begin() + 1;
             block_iter < m_sec_disassembly->getMaximalBlocks().end();
             ++block_iter, ++node_iter) {

            (*node_iter).setMaximalBlock(&(*block_iter));
            if ((*block_iter).getBranch().isDirect()
                && !isValidCodeAddr((*block_iter).getBranch().target())) {
                // a branch to an address outside of executable code
                (*node_iter).setToDataAndInvalidatePredecessors();
                continue;
            }
            auto rev_cfg_node_iter = (node_iter) - 1;
            // check for overlap MB
            for (; rev_cfg_node_iter >= m_sec_cfg.m_cfg.begin();
                   --rev_cfg_node_iter) {
                if ((*rev_cfg_node_iter).maximalBlock()->endAddr() <=
                    (*node_iter).maximalBlock()->addrOfFirstInst()) {
                    // there is no MB overlap
                    break;
                }
//                std::cout << "MaximalBlock: " << (*rev_block_iter).id()
//                    << " Overlaps with : " << (*block_iter).id() << "\n";
                // set pointer to the overlap block
                (*rev_cfg_node_iter).m_overlap_node = (&(*node_iter));
            }
        }
    }
    // second pass for setting successors and predecessors to each CFGNode
    for (auto node_iter = cfg.begin();
         node_iter < cfg.end(); ++node_iter) {
        if ((*node_iter).isData()) {
            continue;
        }
        auto current_block = (*node_iter).maximalBlock();
        if (current_block->getBranch().isDirect()) {
            auto branch_target = current_block->getBranch().target();
            if (!m_sec_disassembly->
                isWithinSectionAddressSpace(branch_target)) {
                // a valid direct branch can happen to an executable section
                // other than this section.
                continue;
            }
            auto succ = findRemoteSuccessor(branch_target);
            if (succ != nullptr && !succ->isData()) {
                (*node_iter).setRemoteSuccessor(succ);
                succ->addRemotePredecessor(&(*node_iter), branch_target);
//                    std::cout << "MaximalBlock: " << (*node_iter).id()
//                        << " Points to: " << (*succ).id() << "\n";
            } else {
                // a direct branch that doesn't target an MB is data
                (*node_iter).setToDataAndInvalidatePredecessors();
            }
        }
    }
    m_sec_cfg.m_valid = true;
}

bool SectionDisassemblyAnalyzerARM::isValidCodeAddr(addr_t addr) const noexcept {
    // XXX: validity should consider alignment of the address
    return (m_exec_addr_start <= addr) && (addr < m_exec_addr_end);
}

CFGNode *SectionDisassemblyAnalyzerARM::findImmediateSuccessor
    (const CFGNode &cfg_node) noexcept {
    // no direct successor to last cfg node
    if (m_sec_cfg.isLast(&cfg_node)) {
        return nullptr;
    }
    auto direct_succ =
        &(*(m_sec_cfg.m_cfg.begin() + cfg_node.id() + 1));
    if (!direct_succ->isData() && direct_succ->maximalBlock()->
        isAddressOfInstruction(cfg_node.maximalBlock()->endAddr())) {
        return direct_succ;
    }
    auto overlap_node = direct_succ->getOverlapNodePtr();
    if (overlap_node != nullptr && !overlap_node->isData()
        && overlap_node->maximalBlock()->
            isAddressOfInstruction(cfg_node.maximalBlock()->endAddr())) {
        return overlap_node;
    }
    return nullptr;
}

CFGNode *SectionDisassemblyAnalyzerARM::findRemoteSuccessor
    (addr_t target) noexcept {

    // binary search to find the remote MB that is targeted.
    if (target < m_exec_addr_start || target > m_exec_addr_end) {
        return nullptr;
    }
    size_t first = 0;
    size_t last = m_sec_disassembly->maximalBlockCount() - 1;
    size_t middle = (first + last) / 2;
    while (middle > first) {
        if (target <
            m_sec_disassembly->maximalBlockAt(middle).addrOfLastInst()) {
            last = middle;
        } else {
            first = middle;
        }
        middle = (first + last) / 2;
    }
    if (m_sec_disassembly->maximalBlockAt(last).isAddressOfInstruction(target)) {
        return m_sec_cfg.ptrToNodeAt(last);
    }
    if (m_sec_disassembly->maximalBlockAt(first).isAddressOfInstruction(target)) {
        return m_sec_cfg.ptrToNodeAt(first);
    }
    // Handle overlap MBs.
    auto overlap_node = m_sec_cfg.getNodeAt(last).getOverlapNode();
    if (overlap_node != nullptr &&
        overlap_node->maximalBlock()->isAddressOfInstruction(target)) {
        return m_sec_cfg.ptrToNodeAt(overlap_node->id());
    }
    return nullptr;
}

const DisassemblyCFG &SectionDisassemblyAnalyzerARM::getCFG() const noexcept {
    return m_sec_cfg;
}

void SectionDisassemblyAnalyzerARM::refineCFG() {
    if (!m_sec_cfg.isValid()) {
        return;
    }
    for (auto node_iter = m_sec_cfg.m_cfg.begin();
         node_iter < m_sec_cfg.m_cfg.end(); ++node_iter) {
        if ((*node_iter).isData())
            continue;
        resolveOverlapBetweenNodes(*node_iter);
        if ((*node_iter).isData())
            continue;
        addConditionalBranchToCFG(*node_iter);
        addCallReturnRelation(*node_iter);
        // find maximally valid BB and resolves conflicts between MBs
        resolveValidBasicBlock((*node_iter));
    }
    recoverSwitchStatements();
}

void SectionDisassemblyAnalyzerARM::resolveOverlapBetweenNodes(CFGNode &node) {
    if (!node.hasOverlapWithOtherNode() || node.getOverlapNode()->isData()) {
        return;
    }
    // resolve overlap between MBs by shrinking the next or converting this to data
    if (node.getOverlapNode()->maximalBlock()->
        coversAddressSpaceOf(node.maximalBlock())) {
        if (calculateNodeWeight(&node) <
            calculateNodeWeight(node.getOverlapNode())) {
            if (m_sec_cfg.previous(node).isAppendableBy(&node)
                && calculateNodeWeight(&m_sec_cfg.previous(node)) > 2) {
                // TODO: alignment should be revisted!!
                // XXX: heuristic applied when this node aligns with previous
                // what if next is one instruction?
                node.getOverlapNodePtr()->
                    setCandidateStartAddr(node.maximalBlock()->endAddr());
            } else {
                node.setToDataAndInvalidatePredecessors();
            }
        }
    } else {
        if (node.getOverlapNodePtr()->
            isCandidateStartAddressValid(node.maximalBlock()->endAddr())) {
            auto nested_overlap =
                node.getOverlapNodePtr()->getOverlapNodePtr();
            if (nested_overlap != nullptr
                && node.isAppendableBy(nested_overlap)) {
                node.getOverlapNodePtr()->setToDataAndInvalidatePredecessors();
            } else {
                node.getOverlapNodePtr()->
                    setCandidateStartAddr(node.maximalBlock()->endAddr());
            }
        } else if (calculateNodeWeight(&node) <
            calculateNodeWeight(node.getOverlapNode())) {
            node.setToDataAndInvalidatePredecessors();
        } else {
            // overlapping node consists of only one instruction?
            node.getOverlapNodePtr()->setToDataAndInvalidatePredecessors();
        }
    }
}

void SectionDisassemblyAnalyzerARM::resolveValidBasicBlock(CFGNode &node) {
    if (!node.isCandidateStartAddressSet()) {
        // with no objections we take the first instruction
        if (node.isPossibleReturn()) {
            node.setCandidateStartAddr
                (node.getPreceedingCallNode()->maximalBlock()->endAddr());
        } else {
            node.setCandidateStartAddr(node.maximalBlock()->addrOfFirstInst());
        }
    }
    if (node.maximalBlock()->getBasicBlocksCount() == 1
        || node.getDirectPredecessors().size() == 0) {
        // nothing more to do
        return;
    }
    std::vector<CFGEdge> valid_predecessors;
    for (auto &pred_iter: node.getDirectPredecessors()) {
        if (!(pred_iter).node()->isData()) {
            valid_predecessors.push_back(pred_iter);
        }
    }
    // The common case where all branches target the same basic block
    for (auto bblock_iter = node.maximalBlock()->getBasicBlocks().begin();
         bblock_iter < node.maximalBlock()->getBasicBlocks().end();
         ++bblock_iter) {
        unsigned target_count = 0;
        for (auto pred_iter = valid_predecessors.cbegin();
             pred_iter < valid_predecessors.cend(); ++pred_iter) {
            for (addr_t addr : (*bblock_iter).getInstructionAddresses()) {
                if ((*pred_iter).targetAddr() == addr) {
                    if ((*pred_iter).targetAddr()
                        < node.getCandidateStartAddr()) {
                        auto overlap_pred =
                            m_sec_cfg.ptrToNodeAt(node.id() - 1);
                        if (calculateNodeWeight((*pred_iter).node()) <
                            calculateNodeWeight(overlap_pred)) {
                            (*pred_iter).node()->setToDataAndInvalidatePredecessors();
                        } else {
                            overlap_pred->setToDataAndInvalidatePredecessors();
                        }
                    }
                    target_count++;
                    // a predecessor-target tuple is unique
                    break;
                }
            }
        }
        if (target_count == valid_predecessors.size()) {
            if (node.getCandidateStartAddr() < (*bblock_iter).startAddr()) {
                // TODO: better handling of conflicts here
                if (node.isPossibleReturn() && valid_predecessors.size() == 1) {
                    valid_predecessors[0].node()->setToDataAndInvalidatePredecessors();
                }
//                node.setCandidateStartAddr((*bblock_iter).startAddr());
            }
            return;
        }
    }
    // No basic block satisfies all targets then conflicts should be resolved
    resolveCFGConflicts(node, valid_predecessors);
}

void SectionDisassemblyAnalyzerARM::resolveCFGConflicts
    (CFGNode &node, const std::vector<CFGEdge> &valid_predecessors) {
    // Conflicts between predecessors needs to be resolved.
//    std::cout << "resolving conflicts for node:" << node.id() << std::endl;
    std::vector<size_t> assigned_predecessors;
    assigned_predecessors.resize(valid_predecessors.size(), 0);
    int valid_bb_idx = 0;
    {
        size_t maximum_weight = 0;
        // find the basic block with maximum weight giving priority to
        // earlier BB.
        for (int i =
            static_cast<int>(node.maximalBlock()->getBasicBlocksCount() - 1);
             i >= 0; --i) {
            size_t current_weight = node.maximalBlock()->
                getBasicBlockAt(i).instructionCount();
            size_t j = 0;
            for (auto pred_iter = valid_predecessors.cbegin();
                 pred_iter < valid_predecessors.cend();
                 ++pred_iter, ++j) {
                // basic block weight = calculate predecessor instruction count
                //                      + instruction count of BB
                auto addrs = node.maximalBlock()->
                    getBasicBlockAt(i).getInstructionAddresses();
                if (std::find(addrs.begin(),
                              addrs.end(),
                              (*pred_iter).targetAddr())
                    != addrs.end()) {
                    assigned_predecessors[j] = static_cast<size_t>(i);
                    current_weight += calculateNodeWeight((*pred_iter).node());
                }
            }
            if (current_weight >= maximum_weight) {
                valid_bb_idx = i;
                maximum_weight = current_weight;
            }
        }
    }
    unsigned j = 0;
    for (auto pred_iter = valid_predecessors.cbegin();
         pred_iter < valid_predecessors.cend(); ++pred_iter, ++j) {
        if (assigned_predecessors[j] != valid_bb_idx) {
            // set predecessor to data
            (*pred_iter).node()->setToDataAndInvalidatePredecessors();
        }
    }
}

void SectionDisassemblyAnalyzerARM::resolveLoadConflicts(CFGNode &node) {
    // A load conflict can happen between an MB_1 and another MB_2 such that
    // MB_1 < MB_2 (comparing start addresses)
    auto pc_relative_loads =
        m_analyzer.getPCRelativeLoadsInstructions(&node);
    for (auto inst_ptr: pc_relative_loads) {
        // get conflict target node
        // compare weights and shrink the node with less weight
        addr_t target = inst_ptr->addr() + 4 +
            inst_ptr->detail().arm.operands[1].mem.disp;
        target = (target >> 2) << 2; // align target to word address
        CFGNode *target_node =
            findCFGNodeAffectedByLoadStartingFrom(node, target);
        if (target_node == nullptr) {
            shortenToCandidateAddressOrSetToData(node, inst_ptr->endAddr());
            continue;
        }
        if (target + 4 <= target_node->getCandidateStartAddr()) {
            continue;
        }
        // XXX: no weight analysis is applied here, that should be handled
        shortenToCandidateAddressOrSetToData((*target_node), target + 4);
//        printf("Node %lu shortens node %lu\n", node.id(), target_node->id());
        if (target_node->isData()) {
            auto next_node =
                m_sec_cfg.ptrToNodeAt(target_node->id() + 1);
            if (next_node->getCandidateStartAddr() < target + 4) {
//                printf("Inner: node %lu shortens node %lu\n",
//                       node.id(),
//                       target_node->id());
                shortenToCandidateAddressOrSetToData(*next_node, target + 4);
            }
        }
    }
}

CFGNode *SectionDisassemblyAnalyzerARM::findCFGNodeAffectedByLoadStartingFrom
    (const CFGNode &node, addr_t target) noexcept {
    // TODO: compare this with another version that does binary search
    if (target < node.maximalBlock()->endAddr()
        || target > m_exec_addr_end) {
        // A PC-relative load can't target its same MB or load an external address
        return nullptr;
    }
    for (auto node_iter = m_sec_cfg.m_cfg.begin() + node.id() + 1;
         node_iter < m_sec_cfg.m_cfg.end(); ++node_iter) {
        // we only care about affected instructions
        if (target <= (*node_iter).maximalBlock()->addrOfLastInst()) {
            return &(*node_iter);
        }
    }
    return nullptr;
}

void SectionDisassemblyAnalyzerARM::shortenToCandidateAddressOrSetToData
    (CFGNode &node, addr_t addr) noexcept {
    if (node.isCandidateStartAddressValid(addr)) {
        node.setCandidateStartAddr(addr);
    } else {
        node.setToDataAndInvalidatePredecessors();
    }
}

void SectionDisassemblyAnalyzerARM::recoverSwitchStatements() {
    std::vector<const CFGNode *> switch_nodes;
    for (auto node_iter = m_sec_cfg.m_cfg.begin();
         node_iter < m_sec_cfg.m_cfg.end(); ++node_iter) {
        if ((*node_iter).isData() || isNotSwitchStatement(*node_iter))
            continue;
        if ((*node_iter).maximalBlock()->
            branchInstruction()->id() == ARM_INS_TBB) {
            switch_nodes.push_back(&(*node_iter));
            recoverTBBSwitchTable((*node_iter));
        }
        if ((*node_iter).maximalBlock()->
            branchInstruction()->id() == ARM_INS_TBH) {
            switch_nodes.push_back(&(*node_iter));
            recoverTBHSwitchTable((*node_iter));
        }
        if ((*node_iter).maximalBlock()->
            branchInstruction()->id() == ARM_INS_LDR
            && (*node_iter).maximalBlock()->
                branchInstruction()->detail().arm.op_count == 2) {
            switch_nodes.push_back(&(*node_iter));
            recoverLDRSwitchTable
                (*node_iter, m_analyzer.recoverLDRSwitchBaseAddr(*node_iter));
        }
    }
    for (const auto node_ptr : switch_nodes) {
        switchTableCleanUp(*node_ptr);
    }
}

bool SectionDisassemblyAnalyzerARM::isNotSwitchStatement
    (const CFGNode &node) const noexcept {
    if (node.maximalBlock()->getBranch().isDirect()
        || node.maximalBlock()->getBranch().isConditional())
        // a switch stmt can't be direct or conditional
        return true;
    if (node.maximalBlock()->branchInstruction()->id() == ARM_INS_POP
        || node.maximalBlock()->branchInstruction()->id() == ARM_INS_BLX
        || node.maximalBlock()->branchInstruction()->id() == ARM_INS_BL
        || node.maximalBlock()->branchInstruction()->id() == ARM_INS_BX) {
        return true;
    }
    return false;
}

void SectionDisassemblyAnalyzerARM::addConditionalBranchToCFG(CFGNode &node) {
    if (!node.maximalBlock()->getBranch().isConditional()) {
        return;
    }
    if (!isConditionalBranchAffectedByNodeOverlap(node)) {
        // a conditional branch should be valid
        auto succ = findImmediateSuccessor(node);
        if (succ != nullptr) {
            node.setImmediateSuccessor(succ);
            succ->addImmediatePredecessor
                (&node, node.maximalBlock()->endAddr());
        } else {
            // a conditional branch without a direct successor is data
            node.setToDataAndInvalidatePredecessors();
        }
    }
}

bool SectionDisassemblyAnalyzerARM::isConditionalBranchAffectedByNodeOverlap
    (const CFGNode &node) const noexcept {
    if (node.maximalBlock()->branchInstruction()->id() == ARM_INS_CBZ
        || node.maximalBlock()->branchInstruction()->id() == ARM_INS_CBNZ) {
        // these instructions are not affected
        return false;
    }
    if (!node.isCandidateStartAddressSet()) {
        // if there was no overlap or branches are not affected by context.
        // additionally larger nodes are not affected (heuristic)
        if (node.maximalBlock()->instructionsCount() == 1)
            // XXX Capstone should be figure out that IT can't affect other BBs.
            return true;
        return false;
    } else {
        for (auto inst_iter =
            node.maximalBlock()->getAllInstructions().cbegin();
             inst_iter < node.maximalBlock()->getAllInstructions().cend();
             inst_iter++) {
            if ((*inst_iter).addr() >= node.getCandidateStartAddr()) {
                return false;
            }
            if ((*inst_iter).id() == ARM_INS_CMP
                || (*inst_iter).id() == ARM_INS_CMN
                || (*inst_iter).id() == ARM_INS_IT) {
                // if there was a conditional execution instruction eliminated
                //  by overlap analysis then we won't consider the block.
                // TODO: check if a conditional execution instruction actually
                // affects the branch instruction
                return true;
            }
        }
    }
    return false;
}

void SectionDisassemblyAnalyzerARM::recoverTBBSwitchTable(CFGNode &node) {
    // assuming TBB is always based on PC
    const addr_t base_addr =
        node.maximalBlock()->branchInstruction()->addr() + 4;
    const uint8_t *code_ptr = m_sec_disassembly->physicalAddrOf(base_addr);
    addr_t minimum_switch_case_addr = m_exec_addr_end;
    addr_t current_addr = base_addr;
    std::unordered_map<addr_t, bool> target_map;
    while (current_addr < minimum_switch_case_addr) {
        addr_t target = base_addr + (*code_ptr) * 2;
        auto insert_result = target_map.insert({target, false});
        // there are many redundancies in a switch table
        if (insert_result.second) {
            if (target < current_addr) {
                break;
            }
            auto target_node = findSwitchTableTarget(target);
            if (target_node == nullptr) {
                // switch table looks padded or not bounded!
                break;
            }
            target_node->setAsSwitchCaseFor(&node, target);
            if (target < minimum_switch_case_addr) {
                minimum_switch_case_addr = target;
            }
        }
        code_ptr++;
        current_addr++;
    }
}

void SectionDisassemblyAnalyzerARM::recoverTBHSwitchTable(CFGNode &node) {
    // assuming TBH is always based on PC
    const addr_t base_addr =
        node.maximalBlock()->branchInstruction()->addr() + 4;
    const uint8_t *code_ptr = m_sec_disassembly->physicalAddrOf(base_addr);
    addr_t minimum_switch_case_addr = m_exec_addr_end;
    addr_t current_addr = base_addr;
    std::unordered_map<addr_t, bool> target_map;
    while (current_addr < minimum_switch_case_addr) {
        addr_t target = base_addr +
            (*(reinterpret_cast<const uint16_t *>(code_ptr))) * 2;
        auto insert_result = target_map.insert({target, false});
        if (insert_result.second) {
            if (target < current_addr) {
                break;
            }
            // there are many redundancies in a switch table
            auto target_node = findSwitchTableTarget(target);
            if (target_node == nullptr) {
                // switch table looks padded or not bounded!
                break;
            }
            target_node->setAsSwitchCaseFor(&node, target);
            if (target < minimum_switch_case_addr) {
                minimum_switch_case_addr = target;
            }
        }
        code_ptr += 2;
        current_addr += 2;
    }
}

void SectionDisassemblyAnalyzerARM::recoverLDRSwitchTable
    (CFGNode &node, const addr_t jump_table_base_addr) {
    const uint8_t *code_ptr = m_sec_disassembly->
        physicalAddrOf(jump_table_base_addr);
    addr_t current_addr = jump_table_base_addr;
    addr_t minimum_switch_case_addr = m_exec_addr_end;
    std::unordered_map<addr_t, bool> target_map;
    while (current_addr < minimum_switch_case_addr) {
        uint32_t target = *(reinterpret_cast<const uint32_t *>(code_ptr))
            & 0xFFFFFFFE;
        auto insert_result = target_map.insert({target, false});
        // there are many redundancies in a switch table
        if (insert_result.second) {
            auto target_node = findSwitchTableTarget(target);
            if (target_node == nullptr) {
                // switch table looks padded or not bounded!
                break;
            }
            target_node->setAsSwitchCaseFor(&node, target);
            if (target < minimum_switch_case_addr
                && target > jump_table_base_addr) {
                // we pick only nodes after the current node since jumping
                // to default case can happen earlier
                minimum_switch_case_addr = target;
            }
        }
        code_ptr += 4;
        current_addr += 4;
    }
}

void SectionDisassemblyAnalyzerARM::switchTableCleanUp
    (const CFGNode &node) {
    for (auto node_iter = m_sec_cfg.m_cfg.begin() + node.id() + 1;
         node_iter <= m_sec_cfg.m_cfg.end();
         ++node_iter) {
        if ((*node_iter).getType() == CFGNodeType::kData) {
            continue;
        }
        if ((*node_iter).getMinTargetAddrOfValidPredecessor() == 0) {
            (*node_iter).setType(CFGNodeType::kData);
//            printf("Switch clean up at node %lu invalidating node %lu\n",
//                   node.id(), (*node_iter).id());
        } else {
            (*node_iter).setCandidateStartAddr
                ((*node_iter).getMinTargetAddrOfValidPredecessor());
            break;
        }
    }
}

CFGNode *SectionDisassemblyAnalyzerARM::findSwitchTableTarget
    (addr_t target_addr) {
    if (target_addr < m_exec_addr_start || target_addr >= m_exec_addr_end) {
        return nullptr;
    }
    // switch tables can branch to an node that precedes current node
    size_t first = 0;
    size_t last = m_sec_disassembly->maximalBlockCount() - 1;
    size_t middle = (first + last) / 2;
    while (middle > first) {
        if (target_addr <
            m_sec_disassembly->maximalBlockAt(middle).addrOfLastInst()) {
            last = middle;
        } else {
            first = middle;
        }
        middle = (first + last) / 2;
    }
    // assuming that switch table targets are valid instructions
    if (m_sec_cfg.ptrToNodeAt(last)->isData()) {
        if (m_sec_cfg.ptrToNodeAt(last)->m_overlap_node != nullptr) {
            return m_sec_cfg.ptrToNodeAt(last)->m_overlap_node;
        }
    } else if (m_sec_disassembly->
        maximalBlockAt(last).isWithinAddressSpace(target_addr)) {
        return m_sec_cfg.ptrToNodeAt(last);
    }
    if (m_sec_disassembly->
        maximalBlockAt(first).isWithinAddressSpace(target_addr)) {
        return m_sec_cfg.ptrToNodeAt(first);
    }
    return nullptr;
}

void SectionDisassemblyAnalyzerARM::buildCallGraph() {
    // recover a map of target addresses and direct call sites
    auto call_sites = recoverDirectCallSites();
    std::sort(call_sites.begin(), call_sites.end());
    // initial call graph where every directly reachable procedure is identified
    //  together with its overestimated address space
    buildInitialCallGraph(call_sites);
    for (auto &proc : m_call_graph.m_graph_vec) {
        buildProcedure(proc);
    }

    // each external call site should be added to call graph as external
    // each internal site should be traversed to check that it's actually a procedure
    // the traversal should return one of the following results
    // valid: all paths return to caller
    // non-return: some

}

void SectionDisassemblyAnalyzerARM::buildProcedure
    (ICFGNode &proc_node) {
    if (proc_node.m_proc_type == ICFGProcedureType::kExternal) {
        // we do not traverse dynamically linked libraries
        return;
    }
    proc_node.m_lr_store_idx =
        m_analyzer.getLRStackStoreIndex(proc_node.m_entry_node);
    if (proc_node.entryNode()->maximalBlock()->getBranch().isConditional()) {
        traverseProcedureNode(proc_node,
                              proc_node.entryNode()->m_immediate_successor,
                              proc_node.entryNode());
    }
    if (proc_node.entryNode()->isCall()) {
        traverseProcedureNode
            (proc_node,
             proc_node.entryNode()->getReturnSuccessorNode(),
             proc_node.entryNode());
    }
    traverseProcedureNode(proc_node,
                          proc_node.entryNode()->m_remote_successor,
                          proc_node.entryNode());
    proc_node.finalize();
    std::cout << std::endl;
    printf("Procedure %s built...\n", proc_node.name().c_str());
    printf("Entry node %lu at: %lx \n",
           proc_node.entryNode()->id(),
           proc_node.entryNode()->getCandidateStartAddr());
    for (auto &exitNodePair : proc_node.m_exit_nodes) {
        switch (exitNodePair.first) {
            case ICFGExitNodeType::kReturn:
                printf("Exit_return node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
                break;
            case ICFGExitNodeType::kCall:
                printf("Exit_call node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
                break;
            case ICFGExitNodeType::kIndirectCall:
                printf("Exit_ind_call node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
                break;
            case ICFGExitNodeType::kInvalidLR:
                printf("Exit_invalid node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
                break;
            case ICFGExitNodeType::kTailCall:
                printf("Exit_tail_call node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
                break;
            case ICFGExitNodeType::kPossibleOverlap:
                printf("Exit_pos_overlap node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
                break;
            default:
                printf("Exit_overlap node %lu at: %lx /",
                       exitNodePair.second->id(),
                       exitNodePair.second->getCandidateStartAddr());
        }
        printf("\n");
    }
    printf("Procedure end.\n");
}

void SectionDisassemblyAnalyzerARM::traverseProcedureNode
    (ICFGNode &proc_node, CFGNode *cfg_node, CFGNode *predecessor) {
    if (cfg_node == nullptr) {
        // a call to an external procedure
//        printf("Exiting at ext succ of %lu\n", predecessor->id());
        if (predecessor->isCall()) {
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kCall, predecessor});
        } else {
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kTailCall, predecessor});
        }
        if (!predecessor->isRoleInProcedureSet())
            predecessor->m_role_in_procedure = CFGNodeRoleInProcedure::kExit;
        return;
    }
    if (cfg_node->isAssignedToProcedure()) {
        if (proc_node.entryAddr() != cfg_node->m_procedure_entry_addr) {
            // visiting a node already assigned to another procedure
//            printf("Exiting at %lu succ of %lu\n",
//                   cfg_node->id(),
//                   predecessor->id());
            if (cfg_node->m_role_in_procedure
                == CFGNodeRoleInProcedure::kEntry) {
                if (predecessor->isCall()) {
                    proc_node.m_exit_nodes.push_back
                        ({ICFGExitNodeType::kCall, predecessor});
                } else {
                    proc_node.m_exit_nodes.push_back
                        ({ICFGExitNodeType::kTailCall, predecessor});
                }
            } else {
                proc_node.m_exit_nodes.push_back
                    ({ICFGExitNodeType::kOverlap, predecessor});
            }
            if (!predecessor->isRoleInProcedureSet())
                predecessor->m_role_in_procedure =
                    CFGNodeRoleInProcedure::kExit;
        }
        return;
    }
    if (!proc_node.isWithinAddressSpace(cfg_node->getCandidateStartAddr())) {
        // visiting a node outside designated address space
//        printf("Exiting at %lu succ of %lu\n",
//               cfg_node->id(),
//               predecessor->id());
        if (cfg_node->m_role_in_procedure == CFGNodeRoleInProcedure::kEntry) {
            if (predecessor->isCall()) {
                proc_node.m_exit_nodes.push_back
                    ({ICFGExitNodeType::kCall, predecessor});
            } else {
                proc_node.m_exit_nodes.push_back
                    ({ICFGExitNodeType::kTailCall, predecessor});
            }
        } else if (cfg_node->isAssignedToProcedure()) {
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kOverlap, predecessor});
        } else {
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kPossibleOverlap, predecessor});
        }
        if (!predecessor->isRoleInProcedureSet())
            predecessor->m_role_in_procedure = CFGNodeRoleInProcedure::kExit;
        return;
    }
    // if invalid stack manipulation return
    if (proc_node.m_lr_store_idx == 0) {
        proc_node.m_lr_store_idx = m_analyzer.getLRStackStoreIndex(cfg_node);
    } else if (m_analyzer.getLRStackStoreIndex(cfg_node) != 0) {
        // doing double stack allocation for LR is not valid
//        printf("Exiting at %lu succ of %lu\n",
//               cfg_node->id(),
//               predecessor->id());
        predecessor->m_role_in_procedure = CFGNodeRoleInProcedure::kExit;
        proc_node.m_exit_nodes.push_back
            ({ICFGExitNodeType::kInvalidLR, predecessor});
    }
    // cfg node is now assigned to this procedure
//    printf("CFG visiting node %lu at loc_%lx succ of %lu\n",
//           cfg_node->id(),
//           cfg_node->getCandidateStartAddr(),
//           predecessor->id());
    cfg_node->m_procedure_entry_addr = proc_node.entryAddr();
    if (cfg_node->maximalBlock()->getBranch().isDirect()) {
        cfg_node->m_role_in_procedure = CFGNodeRoleInProcedure::kBody;
        if (cfg_node->maximalBlock()->getBranch().isConditional()) {
            traverseProcedureNode
                (proc_node, cfg_node->m_immediate_successor, cfg_node);
        }
        traverseProcedureNode
            (proc_node, cfg_node->m_remote_successor, cfg_node);
        if (cfg_node->isCall()) {
            traverseProcedureNode
                (proc_node, cfg_node->getReturnSuccessorNode(), cfg_node);
        }
    } else {
        if (cfg_node->isSwitchStatement()) {
            cfg_node->m_role_in_procedure = CFGNodeRoleInProcedure::kBody;
            for (auto &item : cfg_node->m_indirect_succs) {
                traverseProcedureNode
                    (proc_node, item.node(), cfg_node);
            }
            return;
        }
        cfg_node->m_role_in_procedure = CFGNodeRoleInProcedure::kExit;
        if (cfg_node->isCall()) {
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kIndirectCall, cfg_node});
            traverseProcedureNode
                (proc_node, cfg_node->getReturnSuccessorNode(), cfg_node);
            return;
        }
        if (m_analyzer.isReturn(cfg_node->maximalBlock()->branchInstruction())) {
            // TODO: what if a return doesn't match the same LR
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kReturn, cfg_node});
            return;
        } else {
            proc_node.m_exit_nodes.push_back
                ({ICFGExitNodeType::kTailCall, cfg_node});
        }
    }
}

std::vector<std::pair<addr_t, const CFGNode *>>
SectionDisassemblyAnalyzerARM::recoverDirectCallSites() const noexcept {
    std::vector<std::pair<addr_t, const CFGNode *>> call_sites;
    for (const auto &cfg_node :m_sec_cfg.m_cfg) {
        if (cfg_node.isData()) {
            continue;
        }
        if (cfg_node.isCall()
            && cfg_node.maximalBlock()->getBranch().isDirect()) {
            call_sites.push_back
                (std::make_pair(cfg_node.maximalBlock()->getBranch().target(),
                                &cfg_node));
        }
    }
    return call_sites;
}

void SectionDisassemblyAnalyzerARM::buildInitialCallGraph
    (const AddrCFGNodePairVec &call_sites) noexcept {
    assert(call_sites.size() > 2 && "Too few calls");
    ICFGNode *current_proc;
    {
        current_proc = m_call_graph.addProcedure
            ((*call_sites.begin()).first,
             (*(call_sites.begin())).second->m_remote_successor);
    }
    for (auto call_site_iter = call_sites.cbegin() + 1;
         call_site_iter < call_sites.cend(); ++call_site_iter) {
        if ((*(call_site_iter - 1)).first != (*call_site_iter).first) {
            current_proc->m_end_addr = (*call_site_iter).first;
            current_proc =
                m_call_graph.addProcedure
                    ((*call_site_iter).first,
                     (*call_site_iter).second->m_remote_successor);
        }
    }
    current_proc->m_end_addr = m_exec_addr_end;
}

void SectionDisassemblyAnalyzerARM::addCallReturnRelation(CFGNode &node) {
    if (m_analyzer.isCall(node.maximalBlock()->branchInstruction())) {
        auto succ = findImmediateSuccessor(node);
        if (succ != nullptr) {
            succ->setAsReturnNodeFrom(node);
        }
    }
}
}