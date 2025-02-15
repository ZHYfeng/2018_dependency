/*
 * DataManagement.cpp
 *
 *  Created on: Feb 28, 2019
 *      Author: yhao
 */

#include "DataManagement.h"
#include "llvm/IR/CFG.h"
#include "../DCC/general.h"
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <fstream>
#include <iostream>
#include <sstream>

#define PATH_SIZE 1000000

namespace dra {

    DataManagement::DataManagement() {
        vmOffsets = 0;
        Modules = new dra::DModule();
        Address2BB.reserve(1000000);
    }

    DataManagement::~DataManagement() = default;

    void
    DataManagement::initializeModule(const std::string &objdump, const std::string &AssemblySourceCode,
                                     const std::string &bit_code) {
#if DEBUG_OBJ_DUMP
        std::string obj = objdump.substr(0, objdump.find(".objdump"));
        std::string Cmd = "addr2line -afi -e " + obj;
        std::cout << "o Cmd :" << Cmd << std::endl;
#endif
        Modules->ReadBC(bit_code);
        Modules->ReadObjdump(objdump);
        Modules->ReadAsmSourceCode(AssemblySourceCode);
        BuildAddress2BB();

    }

    void DataManagement::BuildAddress2BB() {
        for (const auto &file : Modules->Function) {
            for (const auto &function : file.second) {
                if (function.second->isRepeat()) {

                } else {
                    for (auto inst : function.second->InstASM) {
                        Address2BB[inst->address] = inst;
                    }
                }
            }
        }
    }

    void DataManagement::getInput(const std::string &coverfile) {

        std::string Line;
        std::ifstream coverFile(coverfile);
        if (coverFile.is_open()) {
            while (getline(coverFile, Line)) {

                DInput *input;
                if (Inputs.find(Line) != Inputs.end()) {
                    input = Inputs[Line];

#if DEBUG_INPUT
                    std::cout << "repeat sig : " << Line << std::endl;
#endif
                    getline(coverFile, Line);
                } else {
                    input = new DInput;
                    Inputs[Line] = input;
                    input->setSig(Line);
                    getline(coverFile, Line);
                    input->setProgram(Line);
                }
                input->Number++;
                getline(coverFile, Line);
                input->setCover(Line, vmOffsets);
            }
        } else {
            std::cerr << "Unable to open coverfile file " << coverfile << "\n";
        }

        for (const auto &i : Inputs) {
            for (auto ii : i.second->MaxCover) {
                cover[ii] = std::time(nullptr);
            }
        }

        setInput();

#if DEBUG_INPUT
        std::cout << "all cover: " << std::endl;
        for(auto i : cover){
            std::cout << std::hex << i << "\n";
        }
#endif
    }

    void DataManagement::getVmOffsets(std::string vm_offsets) {
        std::string Line;
        std::ifstream VmOffsets(vm_offsets);
        if (VmOffsets.is_open()) {
            while (getline(VmOffsets, Line)) {
                this->vmOffsets = std::stoul(Line, nullptr, 10);
                this->vmOffsets = (this->vmOffsets << 32);
            }
        } else {
            std::cerr << "Unable to open vmOffsets file " << vm_offsets << "\n";
        }
    }

    void DataManagement::setInput() {
        for (const auto &it : this->Inputs) {
            std::string sig = it.first;
            for (auto addr : it.second->MaxCover) {
                if (this->Address2BB.find(addr) != this->Address2BB.end()) {
                    this->Address2BB[addr]->update(CoverKind::cover, it.second);
                } else {
#if DEBUG_ERR
                    std::cerr << "un find trace_pc_address " << std::hex << addr << "\n";
#endif
                }

            }
        }
    }

    void DataManagement::setVmOffsets(unsigned long long int vm_offsets) {
        this->vmOffsets = (vm_offsets << 32);
    }

    DInput *DataManagement::getInput(Input *input) {
        const std::string &sig = input->sig();
        const std::string &program = input->program();
#if DEBUG_INPUT
        std::cout << "sig : " << sig << std::endl;
#endif
        DInput *dInput;
        if (Inputs.find(sig) != Inputs.end()) {
            dInput = Inputs[sig];
        } else {
            dInput = new DInput;
            Inputs[sig] = dInput;
            dInput->setSig(sig);
            dInput->setProgram(program);
        }
        dInput->Number++;
        for (const auto &c : input->call()) {
            dInput->idx = c.second.idx();
            for (const auto &a : c.second.address()) {
                unsigned long long int address = a.first;
//                unsigned long long int address = a;
                auto final_address = getRealAddress(address);
                if (this->Address2BB.find(final_address) != this->Address2BB.end()) {
                    this->Address2BB[final_address]->update(CoverKind::cover, dInput);
//                    this->dump_address(final_address);
                } else {
#if DEBUG_ERR
                    std::cerr << "un find trace_pc_address " << std::hex << final_address << "\n";
#endif
                }

                if (this->cover.find(final_address) == this->cover.end()) {
                    auto current_time = std::time(nullptr);
                    // coverage *c = new coverage();
                    // c->time = current_time;
                    // c->address = final_address;
                    this->cover[final_address] = current_time;
                    // this->time.push_back(c);
#if DEBUG
                    std::stringstream stream;
                    stream << std::hex << final_address;
                    outputTime("new cover trace_pc_address " + stream.str());
#endif

//                    if (this->uncover.find(final_address) == this->uncover.end()) {
//
//                    } else {
//                        this->uncover[final_address]->covered = true;
//                        if (input->stat() == StatDependency) {
//                            this->uncover[final_address]->covered_by_dependency = true;
//                        }
//                    }

                } else {
                }
            }
        }

        std::vector<Condition *> temp;
        for (auto ua : dInput->dUncoveredAddress) {
            if (this->cover.find(ua->uncovered_address()) == this->cover.end()) {
                temp.push_back(ua);
            } else {
                delete ua;
            }
        }
        dInput->dUncoveredAddress.clear();
        for (auto ua : temp) {
            dInput->addUncoveredAddress(ua);
        }

        return dInput;
    }

    unsigned long long int DataManagement::getRealAddress(unsigned long long int address) const {
        return address + this->vmOffsets - 5;
    }

    unsigned long long int DataManagement::getSyzkallerAddress(unsigned long long int address) const {
        return address - this->vmOffsets + 5;
    }

    bool DataManagement::isDriver(unsigned long long int address) {

        return true;

        if (this->Address2BB.find(address) != this->Address2BB.end()) {
            if (this->Address2BB[address]->parent != nullptr) {
                auto b = this->Address2BB[address]->parent;
                if (b->parent != nullptr) {
                    auto f = b->parent;
                    if (f->Path.find("block/") == 0) {
                        return true;
                    } else if (f->Path.find("drivers/") == 0) {
                        return true;
                    } else if (f->Path.find("sound/") == 0) {
                        return true;
                    } else if (f->Path.find("kvm/") == 0) {

                    }
                } else {
#if DEBUG_ERR
                    std::cerr << "isDriver not have parent f : " << std::hex << address << "\n";
#endif
                }
            } else {
#if DEBUG_ERR
                std::cerr << "isDriver not have parent bb : " << std::hex << address << "\n";
#endif
            }
        } else {
#if DEBUG_ERR
            std::cerr << "isDriver not find trace_pc_address : " << std::hex << address << "\n";
#endif
        }
        return false;
    }

    void DataManagement::dump_address(unsigned long long int address) {

        if (this->Address2BB.find(address) != this->Address2BB.end()) {
            if (this->Address2BB[address]->parent != nullptr) {
                auto b = this->Address2BB[address]->parent;
                if (b->parent != nullptr) {
                    auto f = b->parent;
                    std::cout << "dump_address path : " << f->Path << "\n";
                    std::cout << "dump_address trace_pc_address : " << address << "\n";
                    std::cout << "dump_address getSyzkallerAddress : " << this->getSyzkallerAddress(address) << "\n";
                } else {
                    std::cerr << "dump_address not have parent f : " << std::hex << address << "\n";
                }
            } else {
                std::cerr << "dump_address not have parent bb : " << std::hex << address << "\n";
            }
        } else {
            std::cerr << "dump_address not find trace_pc_address : " << std::hex << address << "\n";
        }
    }

    void DataManagement::dump_cover() const {
        std::ofstream out_file("cover_uncover.txt",
                               std::ios_base::out | std::ios_base::app);
        auto current_time = std::time(nullptr);
        out_file << std::ctime(&current_time);
        out_file << "this->cover.size() : " << std::dec << this->cover.size() << "\n";
        out_file.close();
    }

    void DataManagement::dump_uncover() {
        std::ofstream out_file("cover_uncover.txt",
                               std::ios_base::out | std::ios_base::app);

        int ud = 0, ug = 0, ucc = 0, ucd = 0;
        for (auto uc : this->uncover) {
            if (uc.second->belong_to_Driver) {
                ud++;
                out_file << "belong to driver " << "\n";
                if (uc.second->related_to_gv) {
                    ug++;
                    out_file << "related to gv " << "\n";
                    if (uc.second->covered) {
                        ucc++;
                        out_file << "be covered " << "\n";
                        if (uc.second->covered_by_dependency) {
                            ucd++;
                            out_file << "be covered by dependency " << "\n";
                        }
                    }
                    out_file << "uc.second->condition_address : " << std::hex << uc.second->condition_address << "\n";
                    out_file << "uc.second->trace_pc_address : " << std::hex << uc.second->address << "\n";
                    out_file << "getSyzkallerAddress : " << std::hex << this->getSyzkallerAddress(uc.second->address)
                             << "\n";
                }
            }
        }

        auto current_time = std::time(nullptr);
        out_file << std::ctime(&current_time);
        out_file << "this->uncover.size() : " << std::dec << this->uncover.size() << "\n";
        out_file << "belong to driver : " << ud << "\n";
        out_file << "related to gv : " << ug << "\n";
        out_file << "be covered : " << ucc << "\n";
        out_file << "be covered by dependency : " << ucd << "\n";

        out_file.close();
    }

    void DataManagement::dump_ctxs(std::vector<llvm::Instruction *> *ctx) {
        std::cout << "call chain : " << std::dec << ctx->size() << "\n";
        for (auto inst : *ctx) {
            dra::dump_inst(inst);
        }
    }

    DBasicBlock *DataManagement::get_DB_from_bb(llvm::BasicBlock *b) const {
        return this->Modules->get_DB_from_bb(b);
    }

    DBasicBlock *DataManagement::get_DB_from_i(llvm::Instruction *i) {
        return this->Modules->get_DB_from_i(i);
    }

    bool DataManagement::check_uncovered_address(Condition *u) {
        bool res = false;
        if (this->Address2BB.find(u->condition_address()) != this->Address2BB.end()) {
            res = true;
        } else {
            std::cerr << "can not find condition_address : " << std::hex << u->condition_address() << std::endl;
        }

        return res;
    }

    void DataManagement::set_condition(Condition *c) const {
        if (c->right_branch_address_size() != c->syzkaller_right_branch_address_size()) {
            c->set_syzkaller_condition_address(this->getSyzkallerAddress(c->condition_address()));
            c->set_syzkaller_uncovered_address(this->getSyzkallerAddress(c->uncovered_address()));
            for (auto a : c->right_branch_address()) {
                c->add_syzkaller_right_branch_address(this->getSyzkallerAddress(a));
//            (*c->mutable_right_branch_address())[(this->getSyzkallerAddress(a.first))] = 0;
            }
//            for (auto a : c->wrong_branch_address()) {
//                c->add_syzkaller_wrong_branch_address(this->getSyzkallerAddress(a));
////            (*c->mutable_wrong_branch_address())[(this->getSyzkallerAddress(a.first))] = 0;
//            }
        }
    }

    uncover_info::uncover_info() : address(0),
                                   belong_to_Driver(false),
                                   related_to_gv(false),
                                   covered(false),
                                   covered_by_dependency(false) {}
} /* namespace dra */

