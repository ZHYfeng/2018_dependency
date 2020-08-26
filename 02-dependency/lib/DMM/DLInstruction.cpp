/*
 * DLInstruction.cpp
 *
 *  Created on: Feb 28, 2019
 *      Author: yhao
 */

#include "DLInstruction.h"

#include <iostream>

#include "DBasicBlock.h"

namespace dra {

    DLInstruction::DLInstruction() :
            i(nullptr), parent(nullptr), Line(0) {
        state = CoverKind::outside;

    }

    DLInstruction::~DLInstruction() = default;

    void DLInstruction::setState(CoverKind kind) {
        if (state == CoverKind::cover && kind < CoverKind::cover) {
            std::cerr << "error InstIR kind" << "\n";
        }
        state = kind;
    }

    void DLInstruction::update(CoverKind kind) {
        setState(kind);
        parent->update(kind, nullptr);
    }

} /* namespace dra */
