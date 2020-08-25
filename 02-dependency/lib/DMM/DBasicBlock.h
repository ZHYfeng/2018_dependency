/*
 * DBasicBlock.h
 *
 *  Created on: Feb 28, 2019
 *      Author: yhao
 */

#ifndef LIB_DRA_BASICBLOCKALL_H_
#define LIB_DRA_BASICBLOCKALL_H_

#include <llvm/IR/BasicBlock.h>
#include <set>
#include <map>
#include <string>
#include <vector>
#include <llvm/Support/GenericDomTree.h>
#include <llvm/IR/Dominators.h>

#include "DAInstruction.h"
#include "DInput.h"
#include "DLInstruction.h"

namespace dra {
    class DFunction;
} /* namespace dra */

namespace llvm {
    class BasicBlock;
} /* namespace llvm */

namespace dra {
    class DBasicBlock {
    public:
        DBasicBlock();

        virtual ~DBasicBlock();

        void InitIRBasicBlock(llvm::BasicBlock *b);

        void setState(CoverKind kind);

        void update(CoverKind kind, DInput *dInput);

        bool inferCoverBB(DInput *dInput, llvm::BasicBlock *b);

        void inferUncoverBB(llvm::BasicBlock *p, llvm::TerminatorInst *end, u_int32_t i) const;

        void inferSuccessors(llvm::BasicBlock *s, llvm::BasicBlock *b);

//        void inferPredecessors(llvm::BasicBlock *b);

//        void inferPredecessorsUncover(llvm::BasicBlock *b, llvm::BasicBlock *Pred);

        void infer();

        void addNewInput(DInput *i);

        bool isAsmSourceCode() const;

        void setAsmSourceCode(bool asmSourceCode);

        bool isIr() const;

        void setIr(bool ir);

        void dump();

        void real_dump(int kind = 0);

        DBasicBlock *get_DB_from_bb(llvm::BasicBlock *b);

        uint32_t get_number_uncovered_instructions() const;

        void get_function_call(std::set<llvm::Function *> &res);

        uint32_t get_arrive_uncovered_instructions(std::map<std::string, dra::DBasicBlock *> &res) const;

        uint32_t get_all_dominator_uncovered_instructions(std::map<std::string, dra::DBasicBlock *> &res) const;

    public:
        bool IR;
        bool AsmSourceCode;

        llvm::BasicBlock *basicBlock;
        DFunction *parent;
        CoverKind state;
        std::string name;
        uint64_t tracr_num;
        uint64_t trace_pc_address{};
        uint64_t trace_cmp_address{};
        uint32_t number_instructions;

        std::vector<DAInstruction *> InstASM;
        std::vector<DLInstruction *> InstIR;

        std::map<DInput *, uint64_t> input;
        DInput *lastInput;

        std::map<dra::DBasicBlock *, uint64_t> arrive;
    };

} /* namespace dra */

#endif /* LIB_DRA_BASICBLOCKALL_H_ */
