/*
 * DependencyControlCenter.cpp
 *
 *  Created on: May 1, 2019
 *      Author: yhao
 */

#include "DependencyControlCenter.h"
#include <chrono>
#include <thread>
#include <utility>
#include <grpcpp/grpcpp.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstrTypes.h>
#include <sstream>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/InlineAsm.h>
#include "general.h"

namespace dra {

    DependencyControlCenter::DependencyControlCenter() {
        dra::outputTime("start_time");
    }

    DependencyControlCenter::~DependencyControlCenter() = default;

    void DependencyControlCenter::init(const std::string &obj_dump, const std::string &assembly,
                                       const std::string &bit_code, const std::string &config,
                                       const std::string &port_address) {

        DM.initializeModule(obj_dump, assembly, bit_code);
        dra::outputTime("initializeModule");
        dra::outputTime("NumberBasicBlock : " + std::to_string(this->DM.Modules->NumberBasicBlock));
        dra::outputTime("NumberBasicBlockReal : " + std::to_string(this->DM.Modules->NumberBasicBlockReal));

        std::ifstream config_json_ifstream(config);
        config_json_ifstream >> this->config_json;

        //Deserialize the static analysis results.
        for (const auto &dev : this->config_json.items()) {
            std::string staticRes;
            staticRes.assign(dev.value()["file_taint"]);
            dra::outputTime("staticRes : " + staticRes);
            for (const auto &p : dev.value()["path_s"].items()) {
                std::string t = p.value();
                dra::outputTime("path : " + t);
            }
            auto sar = new sta::StaticAnalysisResult();
            this->STA_map[dev.key()] = sar;
            sar->initStaticRes(staticRes, &this->DM);
        }

        if (!port_address.empty()) {
            this->port = port_address;
            this->setRPCConnection(this->port);
        }
    }

    void DependencyControlCenter::run() {
        for (;;) {
#if !DEBUG
            dra::outputTime("wait for get newInput");
#endif
            Inputs *newInput = client->GetNewInput();
            if (newInput != nullptr) {
                for (auto &input : *newInput->mutable_input()) {
//                    std::cout << "new input : " << input.sig() << std::endl;
//                    std::cout << input.program() << std::endl;
#if !DEBUG
                    dra::outputTime("new input : " + input.sig());
                    dra::outputTime(input.program());
#endif
                    DInput *dInput = DM.getInput(&input);
                    check_input(dInput);
                }
                newInput->Clear();
                delete newInput;
#if !DEBUG
                dra::outputTime("sleep_for 10s");
#endif
                std::this_thread::sleep_for(std::chrono::seconds(10));
            } else {
                dra::outputTime("sleep_for 60s");
                std::this_thread::sleep_for(std::chrono::seconds(60));
                setRPCConnection(this->port);
            }

//            this->DM.dump_cover();
//            this->DM.dump_uncover();

            this->check_condition();

            this->send_number_basicblock_covered();

            if (client->Check() == nullptr) {
                break;
            }
        }
    }

    void DependencyControlCenter::setRPCConnection(const std::string &grpc_port) {
        this->client = new dra::DependencyRPCClient(
                grpc::CreateChannel(port, grpc::InsecureChannelCredentials()));
        unsigned long long int vmOffsets = client->GetVmOffsets();
        DM.setVmOffsets(vmOffsets);
        this->client->SendNumberBasicBlock(DM.Modules->NumberBasicBlockReal);
        dra::outputTime("GetVmOffsets");
    }

    void DependencyControlCenter::check_input(DInput *dInput) {
#if DEBUG
        std::cout << "dUncoveredAddress size : " << std::dec << dInput->dUncoveredAddress.size()
                  << std::endl;
#endif

        u_int32_t number_conditions = dInput->dConditionAddress.size();
        u_int32_t number_conditions_dependency = 0;
        for (auto c : dInput->dConditionAddress) {
            if (get_write_basicblock(c) != nullptr) {
                number_conditions_dependency++;
            }
        }
#if DEBUG
        std::cout << "number_conditions : " << std::dec << number_conditions << std::endl;
        std::cout << "number_conditions_dependency : " << std::dec << number_conditions_dependency << std::endl;
#endif


        uint64_t i = 0;
        for (auto u : dInput->dUncoveredAddress) {
            i++;
#if DEBUG
            dra::outputTime("uncovered address count : " + std::to_string(i));
#endif

            if (this->DM.check_uncovered_address(u)) {

                auto *dependency = new Dependency();

                unsigned long long int syzkallerConditionAddress = DM.getSyzkallerAddress(u->condition_address());
                unsigned long long int syzkallerUncoveredAddress = DM.getSyzkallerAddress(u->uncovered_address());
#if DEBUG
                outputTime("");
                std::cout << "condition trace_pc_address : " << std::hex << u->condition_address() << "\n";
                std::cout << "uncovered trace_pc_address : " << std::hex << u->uncovered_address() << "\n";
                std::cout << "condition getSyzkallerAddress : " << std::hex << syzkallerConditionAddress << "\n";
                std::cout << "uncovered getSyzkallerAddress : " << std::hex << syzkallerUncoveredAddress << "\n";
#endif
                UncoveredAddress *uncoveredAddress = dependency->mutable_uncovered_address();
                uncoveredAddress->set_condition_address(syzkallerConditionAddress);
                uncoveredAddress->set_uncovered_address(syzkallerUncoveredAddress);
                for (auto a : u->right_branch_address()) {
                    uncoveredAddress->add_right_branch_address(DM.getSyzkallerAddress(a));
                }

                if (this->DM.Address2BB.find(u->uncovered_address()) != this->DM.Address2BB.end()) {
                    DBasicBlock *db = DM.Address2BB[u->uncovered_address()]->parent;
                    //                    std::set<llvm::BasicBlock *> bbs;
                    //                    this->STA._get_all_successors(db->basicBlock, bbs);
                    //                    uint32_t bbcount = bbs.size();
                    std::map<std::string, dra::DBasicBlock *> temp;
                    uncoveredAddress->set_number_arrive_basicblocks(db->get_arrive_uncovered_instructions(temp));
                    uncoveredAddress->set_number_dominator_instructions(
                            db->get_all_dominator_uncovered_instructions(temp));
                }

                Input *input = dependency->mutable_input();
                input->set_sig(dInput->sig);
                input->set_program(dInput->program);
                input->set_number_conditions(number_conditions);
                input->set_number_conditions_dependency(number_conditions_dependency);
                (*input->mutable_uncovered_address())[syzkallerUncoveredAddress] = u->idx();

                sta::MODS *write_basicblock = this->get_write_basicblock(u);
                if (write_basicblock == nullptr) {
                    uncoveredAddress->set_kind(UncoveredAddressKind::UncoveredAddressInputRelated);
                } else if (write_basicblock->empty()) {
                    uncoveredAddress->set_kind(UncoveredAddressKind::UncoveredAddressDependencyRelated);
                } else if (!write_basicblock->empty()) {
                    uncoveredAddress->set_kind(UncoveredAddressKind::UncoveredAddressDependencyRelated);

                    (*uncoveredAddress->mutable_input())[dInput->sig] = u->idx();

                    set_runtime_data(uncoveredAddress->mutable_run_time_date(), input->program(), u->idx(),
                                     syzkallerConditionAddress, syzkallerUncoveredAddress);

                    for (auto &x : *write_basicblock) {
                        WriteAddress *writeAddress = dependency->add_write_address();
                        write_basic_block_to_address(x, u, writeAddress);
                        auto *waa = new writeAddressAttributes();
                        write_basic_block_to_adttributes(x, waa);
                        (*uncoveredAddress->mutable_write_address())[waa->write_address()] = *waa;
                        (*writeAddress->mutable_uncovered_address())[syzkallerUncoveredAddress] = *waa;
                        set_runtime_data(writeAddress->mutable_run_time_date(), input->program(), u->idx(),
                                         syzkallerConditionAddress, syzkallerUncoveredAddress);
                    }

                }
                this->send_dependency(dependency);
                dependency->Clear();
                delete dependency;
            } else {

            }
        }
    }

    void DependencyControlCenter::send_dependency(Dependency *dependency) {
        if (dependency != nullptr) {
#if DEBUG_RPC
            auto ua = dependency->uncovered_address();
            std::cout << "uncover condition address : " << std::hex << ua.condition_address() << std::endl;
            for (auto wa : dependency->write_address())
            {
                std::cout << "wa program : " << std::endl;
                std::cout << wa.run_time_date().program();
            }
            std::cout << "dependency size : " << dependency->ByteSizeLong() << std::endl;
#endif
            if (dependency->ByteSizeLong() < 0x7fffffff) {
                auto replay = client->SendDependency(*dependency);
                delete replay;
            } else {
                std::cout << "dependency is too big : " << dependency->ByteSizeLong() << std::endl;
            }
        } else {
        }
    }

    void DependencyControlCenter::set_runtime_data(runTimeData *r, const std::string &program, uint32_t idx,
                                                   uint32_t condition, uint32_t address) {
        r->set_program(program);
        r->set_task_status(taskStatus::untested);
        r->set_recursive_count(0);
        r->set_idx(idx);
        r->set_checkcondition(false);
        r->set_condition_address(condition);
        r->set_checkaddress(false);
        r->set_address(address);
        r->mutable_right_branch_address();
    }

    void DependencyControlCenter::write_basic_block_to_address(sta::Mod *write_basicblock, Condition *condition,
                                                               WriteAddress *writeAddress) {

        DBasicBlock *db = this->DM.get_DB_from_bb(write_basicblock->B);
        unsigned int write_address = DM.getSyzkallerAddress(db->trace_pc_address);
#if DEBUG
        dra::outputTime("write basicblock : ");
        db->real_dump();
#endif

        std::vector<sta::cmd_ctx *> *cmd_ctx = write_basicblock->get_cmd_ctx();
        for (auto c : *cmd_ctx) {
#if DEBUG
            std::cout << "cmd hex: " << std::hex << c->cmd << "\n";
            this->DM.dump_ctxs(&c->ctx);
#endif
            auto ctx = c->ctx;
            auto inst = ctx.begin();
            std::string function_name = getFunctionName((*inst)->getParent()->getParent());
            std::string file_operations;
            std::string kind;
            this->getFileOperations(&function_name, &file_operations, &kind);
            int index = 0;
            for (u_int i = file_operations_kind_MIN; i < file_operations_kind_MAX; i++) {
                if (file_operations_kind_Name(static_cast<file_operations_kind>(i)) == kind) {
                    index = i;
                    break;
                }
            }
            (*writeAddress->mutable_file_operations_function())[file_operations] = 1 << index;

        }

        writeAddress->set_write_address(write_address);
        writeAddress->mutable_run_time_date();
        if (write_basicblock->is_trait_fixed()) {
            writeAddress->set_kind(WriteStatementConstant);
        } else {
            writeAddress->set_kind(WriteStatementNonconstant);
        }

        //        auto function_name = "ioctl";
        //        std::cout << "for (auto c : *cmd_ctx) {" << std::endl;
        //        for (auto c : *cmd_ctx) {
        //            std::cout << "for (auto c : *cmd_ctx) {" << std::endl;
        //            Syscall *write_syscall = writeAddress->add_write_syscall();
        //
        ////            (*writeAddress->mutable_write_syscall())[write_address] = *write_syscall;
        //
        //            write_syscall->set_name(function_name);
        //            write_syscall->set_cmd(c->cmd);
        //            write_syscall->mutable_run_time_date();
        //
        //
        //            auto mm = write_syscall->mutable_critical_condition();
        //            bool parity = false;
        //            Condition *indirect_call = nullptr;
        //            for (auto i : c->ctx) {
        //                parity = !parity;
        //                if (parity) {
        //                    auto db = this->DM.get_DB_from_i(i);
        //                    if (db != nullptr) {
        //                        db->parent->compute_arrive();
        //                        if (indirect_call != nullptr) {
        //                            indirect_call->add_right_branch_address(db->trace_pc_address);
        ////                            (*indirect_call->mutable_right_branch_address())[db->trace_pc_address] = 0;
        //
        //                            this->DM.set_condition(indirect_call);
        //                            auto ca = indirect_call->syzkaller_condition_address();
        //                            (*mm)[ca] = *indirect_call;
        //                        }
        //                    }
        //                } else {
        //                    auto db = this->DM.get_DB_from_i(i);
        //                    if (db != nullptr) {
        //                        auto cc = db->critical_condition;
        //                        for (auto ccc : cc) {
        //                            this->DM.set_condition(ccc.second);
        //                            auto ca = ccc.second->syzkaller_condition_address();
        //                            (*mm)[ca] = *ccc.second;
        //                        }
        //                        indirect_call = new Condition();
        //                        indirect_call->set_condition_address(db->trace_pc_address);
        //                    }
        //                }
        //            }
        //        }

        for (auto i : db->input) {
            (*writeAddress->mutable_input())[i.first->sig] = i.second;
        }
    }

    void
    DependencyControlCenter::write_basic_block_to_adttributes(sta::Mod *write_basicblock, writeAddressAttributes *waa) {
        DBasicBlock *db = this->DM.get_DB_from_bb(write_basicblock->B);
        unsigned int write_address = DM.getSyzkallerAddress(db->trace_pc_address);
        waa->set_write_address(write_address);
        waa->set_repeat(write_basicblock->repeat);
        waa->set_prio(write_basicblock->prio + 100);
    }

    void DependencyControlCenter::check_condition() {
        dra::Conditions *cs = client->GetCondition();
        if (cs != nullptr) {
            for (auto condition : *cs->mutable_condition()) {
                sta::MODS *write_basicblock = get_write_basicblock(&condition);
                if (write_basicblock == nullptr) {
                } else {
                    auto *wa = new WriteAddresses();
                    wa->set_allocated_condition(&condition);
                    for (auto &x : *write_basicblock) {
                        WriteAddress *writeAddress = wa->add_write_address();
                        write_basic_block_to_address(x, &condition, writeAddress);
                    }
                    send_write_address(wa);
                }
            }
            cs->Clear();
        } else {
        }
    }

    void DependencyControlCenter::send_write_address(WriteAddresses *writeAddress) {
        if (writeAddress != nullptr) {
#if DEBUG_RPC
            std::cout << "send_write_address : " << std::hex << writeAddress->condition().condition_address()
                      << std::endl;
#endif
            client->SendWriteAddress(*writeAddress);
        } else {
        }
    }


    void DependencyControlCenter::getFileOperations(std::string *function_name, std::string *file_operations,
                                                    std::string *kind) {
        for (const auto &f1 : this->config_json.items()) {
            for (const auto &f2 : f1.value()["function"].items()) {
                for (const auto &f3 : f2.value().items()) {
                    if (*function_name == f3.value()["name"]) {
                        file_operations->assign(f2.key());
                        kind->assign(f3.key());
                    }
                }
            }
        }
    }

    void dra::DependencyControlCenter::test_sta() {
        auto f = this->DM.Modules->Function["block/blk-core.c"]["blk_flush_plug_list"];
        for (const auto &B : f->BasicBlock) {
            auto b = B.second->basicBlock;
            std::cout << "b name : " << B.second->name << std::endl;
            auto sta = this->getStaticAnalysisResult(B.second->parent->Path);
            if (sta == nullptr) {
                continue;
            }
            sta::MODS *allBasicblock = sta->GetAllGlobalWriteBBs(b, true);
            if (allBasicblock == nullptr) {
                // no taint or out side
                std::cout << "allBasicblock == nullptr" << std::endl;
            } else if (allBasicblock->empty()) {
                // unrelated to gv
                std::cout << "allBasicblock->size() == 0" << std::endl;
            } else if (!allBasicblock->empty()) {
                std::cout << "allBasicblock != nullptr && allBasicblock->size() != 0" << std::endl;
            }
        }

        exit(0);
    }

    void dra::DependencyControlCenter::test_rpc() {

        exit(0);
    }

    void dra::DependencyControlCenter::test() {

        for (const auto &ff : this->DM.Modules->Function) {
            for (const auto &f : ff.second) {
                for (auto b : f.second->BasicBlock) {
                    b.second->real_dump();
                }
            }
        }

        exit(0);
    }

    sta::StaticAnalysisResult *DependencyControlCenter::getStaticAnalysisResult(const std::string &path) {
        for (const auto &dev : this->config_json.items()) {
            auto path_s = dev.value()["path_s"];
            for (const auto &pp : path_s.items()) {
                if (path.find(pp.value()) != std::string::npos) {
                    if (this->STA_map.find(dev.key()) != this->STA_map.end()) {
                        return this->STA_map[dev.key()];
                    } else {
                        std::cerr << "can not find static analysis result for dev : " << dev.key() << std::endl;
                    }
                }
            }
        }
        std::cerr << "can not find static analysis result for path : " << path << std::endl;
        return nullptr;
    }

    void DependencyControlCenter::check_all_condition_() {
        auto *coutbuf = std::cout.rdbuf();
        std::ofstream out("address.txt");
        std::cout.rdbuf(out.rdbuf());
        for (auto &f : *this->DM.Modules->module) {
            std::cerr << "function name : " << f.getName().str() << std::endl;
            std::string Path = dra::getFileName(&f);
            std::cerr << "function path : " << Path << std::endl;
            auto sta = this->getStaticAnalysisResult(Path);
            if (sta == nullptr) {
                continue;
            } else {
                std::cerr << "find sta : " << Path << std::endl;
            }
            auto df = this->DM.Modules->get_DF_from_f(&f);
            if (df == nullptr) {
                std::cerr << "not find DF" << std::endl;
                continue;
            }
//            df->dump();
            for (auto &bb : df->BasicBlock) {

                if (bb.second->trace_pc_address == 0) {
//                    std::cerr << "not find trace_pc_address" << std::endl;
//                    bb.second->dump();
                    continue;
                }
                auto fbb = getFinalBB(bb.second->basicBlock);
                auto inst = fbb->getTerminator();
                if (inst->getNumSuccessors() > 1) {
                    sta::MODS *write_basicblock = sta->GetAllGlobalWriteBBs(fbb, 0);
                    if (write_basicblock == nullptr) {
                        // no taint or out side
#if DEBUG
                        dra::outputTime("allBasicblock == nullptr");
                        p->real_dump();
#endif
                        std::cout << std::hex << bb.second->trace_pc_address;
                        for (int i = 0; i < inst->getNumSuccessors(); i++) {
                            std::cout << "&" << this->DM.get_DB_from_bb(inst->getSuccessor(i))->trace_pc_address;
                        }
                        std::cout << std::endl;
                        std::cerr << "write_basicblock == nullptr" << std::endl;
                    } else if (write_basicblock->empty()) {
                        // unrelated to gv
#if DEBUG
                        dra::outputTime("allBasicblock->size() == 0");
                        p->dump();
#endif
                        std::cerr << "write_basicblock->empty()" << std::endl;
                    } else if (!write_basicblock->empty()) {
#if DEBUG
                        dra::outputTime("get useful static analysis result : " + std::to_string(write_basicblock->size()));
#endif
                        std::cerr << "!write_basicblock->empty()" << std::endl;
                    }
                }
            }
        }
        std::cout.rdbuf(coutbuf);
        out.close();
    }

    void DependencyControlCenter::send_number_basicblock_covered() {
        this->client->SendNumberBasicBlockCovered(DM.Modules->NumberBasicBlockCovered);
    }


    sta::MODS *DependencyControlCenter::get_write_basicblock(Condition *u) {
        int64_t successor = u->successor();
        int64_t idx;
        if (successor == 1) {
            idx = 0;
        } else if (successor == 2) {
            idx = 1;
        } else {
            idx = 0;
#if DEBUG_ERR && DEBUG
            std::cerr << "switch case : " << std::hex << successor << std::endl;
#endif
        }
        return get_write_basicblock(u->condition_address(), idx);

    }


    sta::MODS *DependencyControlCenter::get_write_basicblock(u_int64_t address, u_int32_t idx) {
        dra::DBasicBlock *dbb;
        if (this->DM.Address2BB.find(address) != this->DM.Address2BB.end()) {
            dbb = DM.Address2BB[address]->parent;
#if DEBUG
            dbb->dump();
#endif
            return get_write_basicblock(dbb, idx);

        } else {
            return nullptr;
        }
    }

    sta::MODS *DependencyControlCenter::get_write_basicblock(dra::DBasicBlock *db, u_int32_t idx) {
        sta::MODS *res = nullptr;
        auto *bb = dra::getFinalBB(db->basicBlock);


#if DEBUG
        dra::outputTime("GetAllGlobalWriteBBs : ");
#endif
        if ((this->staticResult.find(bb) != this->staticResult.end()) &&
            (this->staticResult[bb].find(idx) != this->staticResult[bb].end())) {
            res = this->staticResult[bb][idx];
#if DEBUG
            dra::outputTime("get useful static analysis result from cache");
#endif
        } else {
            auto sta = this->getStaticAnalysisResult(db->parent->Path);
            if (sta == nullptr) {
                return res;
            }
            sta::MODS *write_basicblock = sta->GetAllGlobalWriteBBs(bb, idx);
            if (write_basicblock == nullptr) {
                // no taint or out side
#if DEBUG
                dra::outputTime("allBasicblock == nullptr");
                db->real_dump();
#endif
            } else if (write_basicblock->empty()) {
                // related to gv but can not find write statements
                res = write_basicblock;
#if DEBUG
                dra::outputTime("allBasicblock->size() == 0");
                db->dump();
#endif
            } else if (!write_basicblock->empty()) {
                // related to gv and find write statements
                res = write_basicblock;
#if DEBUG
                dra::outputTime("get useful static analysis result : " + std::to_string(write_basicblock->size()));
#endif
            }

            this->staticResult[bb].insert(std::pair<uint64_t, sta::MODS *>(idx, res));
        }

        return res;
    }

    void DependencyControlCenter::check_uncovered_addresses_dependnency(const std::string &file) {

        std::stringstream ss;

        std::ifstream conditions(file);
        std::string Line;

        std::ofstream ND("conditionND.txt");
        std::ofstream DN("conditionDN.txt");
        std::ofstream D("conditionD.txt");

        std::map<std::string, dra::DBasicBlock *> Uncover;
        std::map<std::string, dra::DBasicBlock *> DependencyUncover;
        std::map<std::string, dra::DBasicBlock *> NotDependencyUncover;

        std::map<std::string, dra::DBasicBlock *> DUncover;
        std::map<std::string, dra::DBasicBlock *> DDependencyUncover;
        std::map<std::string, dra::DBasicBlock *> DNotDependencyUncover;

        auto *coutbuf = std::cout.rdbuf();
        if (conditions.is_open()) {
            while (getline(conditions, Line)) {
                uint64_t condition_address = 0, unconvered_address = 0;
                uint64_t s = Line.find('&');
                if (s < Line.size()) {
                    ss.str("");
                    for (unsigned long i = 0; i < s; i++) {
                        ss << Line.at(i);
                    }
                    condition_address = std::stoul(ss.str(), nullptr, 16);
                    ss.str("");
                    for (unsigned long i = s + 1; i < Line.size(); i++) {
                        ss << Line.at(i);
                    }
                    unconvered_address = std::stoul(ss.str(), nullptr, 16);

                    std::stringstream stream;
                    stream << std::hex << unconvered_address;
                    std::string result(stream.str());
                    std::ofstream out("0x" + result + ".txt", std::ios_base::app);
                    std::cout << "0x" + result + ".txt" << std::endl;
                    std::cout.rdbuf(out.rdbuf());

                    std::cout << "# Uncovered address address : 0x" << std::hex << unconvered_address << std::endl;
                    DBasicBlock *db_ua;
                    if (this->DM.Address2BB.find(unconvered_address) != this->DM.Address2BB.end()) {
                        db_ua = DM.Address2BB[unconvered_address]->parent;
                        if (db_ua == nullptr) {
                            std::cout << "db_ua == nullptr" << std::endl;
                            continue;
                        } else {
                            db_ua->real_dump(1);
                        }
                    }

                    std::cout << "# condition address : 0x" << std::hex << condition_address << std::endl;
                    if (this->DM.Address2BB.find(condition_address) != this->DM.Address2BB.end()) {
                        DBasicBlock *db = DM.Address2BB[condition_address]->parent;
                        if (db == nullptr) {
                            std::cout << "db == nullptr" << std::endl;
                            continue;
                        } else {
                            db->real_dump(0);

                            sta::MODS *write_basicblock = get_write_basicblock(db);
                            if (write_basicblock == nullptr) {
                                std::cout << "# no taint or out side" << std::endl;


                                ND << "0x" << std::hex << unconvered_address << "@"
                                   << dra::dump_inst_booltin(
                                           getRealBB(db_ua->basicBlock)->getFirstNonPHIOrDbgOrLifetime()) << "@";
                                ND << "0x" << std::hex << condition_address << "@"
                                   << dra::dump_inst_booltin(getFinalBB(db->basicBlock)->getTerminator()) << "@";
                                ND << getFunctionName(db->basicBlock->getParent()) << "@" << db->name << "@";
                                ND << "\n";

                                db->get_arrive_uncovered_instructions(NotDependencyUncover);
                                db->get_all_dominator_uncovered_instructions(DNotDependencyUncover);

                            } else if (write_basicblock->empty()) {
                                std::cout << "# related to gv but not find write statement" << std::endl;

                                DN << "0x" << std::hex << unconvered_address << "@"
                                   << dra::dump_inst_booltin(
                                           getRealBB(db_ua->basicBlock)->getFirstNonPHIOrDbgOrLifetime()) << "@";
                                DN << "0x" << std::hex << condition_address << "@"
                                   << dra::dump_inst_booltin(getFinalBB(db->basicBlock)->getTerminator()) << "@";
                                DN << getFunctionName(db->basicBlock->getParent()) << "@" << db->name << "@";
                                DN << "\n";

                                db->get_arrive_uncovered_instructions(DependencyUncover);
                                db->get_all_dominator_uncovered_instructions(DDependencyUncover);

                            } else if (!write_basicblock->empty()) {
                                std::cout << "# write address : " << write_basicblock->size() << std::endl;

                                D << "0x" << std::hex << unconvered_address << "@"
                                  << dra::dump_inst_booltin(
                                          getRealBB(db_ua->basicBlock)->getFirstNonPHIOrDbgOrLifetime()) << "@";
                                D << "0x" << std::hex << condition_address << "@"
                                  << dra::dump_inst_booltin(getFinalBB(db->basicBlock)->getTerminator()) << "@";
                                D << getFunctionName(db->basicBlock->getParent()) << "@" << db->name << "@";
                                D << "\n";

                                for (auto &x : *write_basicblock) {
                                    DBasicBlock *tdb = this->DM.get_DB_from_bb(x->B);
                                    tdb->real_dump(2);
                                    std::cout << "repeat : " << x->repeat << std::endl;
                                    std::cout << "priority : " << x->prio + 100 << std::endl;
                                    std::vector<sta::cmd_ctx *> *cmd_ctx = x->get_cmd_ctx();
                                    for (auto c : *cmd_ctx) {
                                        for (auto cmd : c->cmd) {
                                            std::cout << "cmd hex: " << std::hex << cmd << "\n";
                                        }
                                        dra::DataManagement::dump_ctxs(&c->ctx);
                                        auto ctx = c->ctx;
                                        auto inst = ctx.begin();
                                        std::string funtion_name = getFunctionName((*inst)->getParent()->getParent());
                                        std::string file_operations;
                                        std::string kind;
                                        this->getFileOperations(&funtion_name, &file_operations, &kind);
                                        int index = 0;
                                        for (int i = file_operations_kind_MIN; i < file_operations_kind_MAX; i++) {
                                            if (file_operations_kind_Name(static_cast<file_operations_kind>(i)) ==
                                                kind) {
                                                index = i;
                                                break;
                                            }
                                        }
                                        std::cout << "funtion_name : " << funtion_name << std::endl;
                                        std::cout << "file_operations : " << file_operations << std::endl;
                                        std::cout << "kind : " << kind << std::endl;
                                        std::cout << "index : " << index << std::endl;
                                    }
                                    std::cout << "--------------------------------------------" << std::endl;
                                    D << " @ @" << "0x" << tdb->trace_pc_address << "@"
                                      << dra::dump_inst_booltin(
                                              getRealBB(tdb->basicBlock)->getFirstNonPHIOrDbgOrLifetime()) << "@"
                                      << x->is_trait_fixed() << "@";
                                    D << "\n";
                                }

                                db_ua->get_arrive_uncovered_instructions(DependencyUncover);
                                db_ua->get_all_dominator_uncovered_instructions(DDependencyUncover);

                            }
                        }
                    }
                    std::cout.rdbuf(coutbuf);
                    out.close();
                }
            }
        }
        conditions.close();
        ND.close();
        DN.close();
        D.close();

        for (auto db : DependencyUncover) {
            Uncover.insert(db);
        }
        for (auto db : NotDependencyUncover) {
            Uncover.insert(db);
        }

        for (auto db : DDependencyUncover) {
            DUncover.insert(db);
        }
        for (auto db : DNotDependencyUncover) {
            DUncover.insert(db);
        }

        FILE *fp;
        fp = fopen("statistic.txt", "a+");
        uint64_t number;
        number = 0;
        for (const auto &db : Uncover) {
            number += db.second->get_number_uncovered_instructions();
        }
        fprintf(fp, "Uncover@%lu@%lu@\n", Uncover.size(), number);
        number = 0;
        for (const auto &db : DependencyUncover) {
            number += db.second->get_number_uncovered_instructions();
        }
        fprintf(fp, "DependencyUncover@%lu@%lu@\n", DependencyUncover.size(), number);
        number = 0;
        for (const auto &db : NotDependencyUncover) {
            number += db.second->get_number_uncovered_instructions();
        }
        fprintf(fp, "NotDependencyUncover@%lu@%lu@\n", NotDependencyUncover.size(), number);

        number = 0;
        for (const auto &db : DUncover) {
            number += db.second->get_number_uncovered_instructions();
        }
        fprintf(fp, "DUncover@%lu@%lu@\n", DUncover.size(), number);
        number = 0;
        for (const auto &db : DDependencyUncover) {
            number += db.second->get_number_uncovered_instructions();
        }
        fprintf(fp, "DDependencyUncover@%lu@%lu@\n", DDependencyUncover.size(), number);
        number = 0;
        for (const auto &db : DNotDependencyUncover) {
            number += db.second->get_number_uncovered_instructions();
        }
        fprintf(fp, "DNotDependencyUncover@%lu@%lu@\n", DNotDependencyUncover.size(), number);
        fclose(fp);

    }

    void DependencyControlCenter::check_write_addresses_dependency(const std::string &file) {

        std::ifstream write(file);
        std::string Line;

        float_t dependency = 0;
        float_t not_dependency = 0;
        float_t other = 0;

        if (write.is_open()) {
            while (getline(write, Line)) {
                uint64_t write_address = std::stoul(Line, nullptr, 16);

                if (this->DM.Address2BB.find(write_address) != this->DM.Address2BB.end()) {
                    DBasicBlock *db = DM.Address2BB[write_address]->parent;
                    if (db == nullptr) {
                        std::cout << "db == nullptr" << std::endl;
                        other++;
                        continue;
                    } else {
                        if (this->is_dependency(db, 0)) {
                            dependency++;
                        } else {
                            not_dependency++;
                        }
                    }
                    FILE *fp;
                    fp = fopen("statistic.txt", "a+");
                    float_t total = dependency + not_dependency + other;
                    if (total == 0) {
                        fprintf(fp, "UncoveredWS@%.2f@%.2f@%.2f@%.2f@%.2f@\n", total, dependency, 1.0, not_dependency,
                                other);
                    } else {
                        fprintf(fp, "UncoveredWS@%.2f@%.2f@%.2f@%.2f@%.2f@\n", total, dependency, dependency / total,
                                not_dependency, other);
                    }
                    fclose(fp);

                }
            }
        }
        write.close();


        // char buf[1024];
        // std::sprintf(buf, "%.2f@%.2f@%.2f@%.2f@%.2f@\n",total,dependency,dependency * 100 / total,not_dependency,other);
        // std::ofstream result("statistic.txt");
        // result << std::string(buf);
    }

    bool DependencyControlCenter::is_dependency(dra::DBasicBlock *db, u_int64_t count) {
        if (count > 200) {
            return false;
        }
        for (auto *Pred : llvm::predecessors(getRealBB(db->basicBlock))) {
            auto db1 = this->DM.get_DB_from_bb(Pred);
            if (this->get_write_basicblock(db1) == nullptr) {
                if (this->is_dependency(db1, count + 1)) {
                    return true;
                }
            } else {
                return true;
            }
        }
        return false;
    }

    void DependencyControlCenter::check_coverage(const std::string &file) {
        std::ifstream write(file);
        std::string Line;

        float_t coverage = 0;

        if (write.is_open()) {
            while (getline(write, Line)) {
                uint64_t write_address = std::stoul(Line, nullptr, 16);

                if (this->DM.Address2BB.find(write_address) != this->DM.Address2BB.end()) {
                    auto DInst = DM.Address2BB[write_address];
                    DInst->update(CoverKind::cover, nullptr);
                    DBasicBlock *db = DInst->parent;
                    if (db == nullptr) {
                        std::cout << "db == nullptr" << std::endl;
                        continue;
                    } else {
                        coverage++;
                    }
                }
            }
        }
        write.close();

        uint64_t count = 0;
        for (const auto &temp : this->DM.Modules->Function) {
            for (const auto &df : temp.second) {
                if (df.second->state > CoverKind::outside && df.second->isIR()) {
                    count += df.second->NumberBasicBlockReal;
                }
            }
        }

        FILE *fp;
        fp = fopen("statistic.txt", "a+");
        fprintf(fp, "union@%.2f@\n", coverage);
        fprintf(fp, "coverage@%lu@\n", count);
        fclose(fp);

        std::ofstream UF("UncoveredFunctions.txt");
        for (const auto &temp : this->DM.Modules->Function) {
            for (const auto &df : temp.second) {
                if (df.second->state > CoverKind::outside && df.second->state < CoverKind::cover && df.second->isIR()) {
                    UF << df.second->FunctionName << "\n";
                }
            }
        }
        UF.close();

        std::ofstream OF("OutsideFunctions.txt");
        for (const auto &temp : this->DM.Modules->Function) {
            for (const auto &df : temp.second) {
                if (df.second->state <= CoverKind::outside && df.second->isIR()) {
                    OF << df.second->FunctionName << "\n";
                }
            }
        }
        OF.close();
    }

    void DependencyControlCenter::check_control_dependency(const std::string &file) {
        std::ifstream not_dependency(file);
        std::string Line;
        std::string delimiter = "@";
        uint64_t pos_start = 0;
        uint64_t pos_end = 0;
        std::ofstream control_dependency("control_dependency.txt");
        uint64_t condition_address;
        uint64_t uncovered_address;

        std::cout << "check_control_dependency" << std::endl;

        auto check_control_dependency = [&, this]() {
            control_dependency << "@0x" << std::hex << uncovered_address;
            if (this->DM.Address2BB.find(condition_address) != this->DM.Address2BB.end()) {
                DBasicBlock *db = DM.Address2BB[condition_address]->parent;
                std::map<std::string, dra::DBasicBlock *> temp1;
                control_dependency << "@" << std::dec << db->get_all_dominator_uncovered_instructions(temp1);;
                control_dependency << "@" << std::dec << temp1.size();

                if (db == nullptr) {
                    goto error;
                } else {

                    std::string str;
                    llvm::raw_string_ostream dump(str);
                    db->basicBlock->print(dump);
//                    control_dependency << str << std::endl;

                    sta::MODS *write_basicblock = get_write_basicblock(db);
                    if (write_basicblock == nullptr) {

                        uint64_t indirect = 0;
                        uint64_t call = 0;

                        for (auto it: predecessors(db->basicBlock)) {
                            auto temp = this->DM.get_DB_from_bb(it);
                            sta::MODS *temp_write_basicblock = get_write_basicblock(temp);
                            if (temp_write_basicblock != nullptr) {
                                control_dependency << "@it" << std::endl;
                                return;
                            }
                            for (auto itt : predecessors(temp->basicBlock)) {
                                temp = this->DM.get_DB_from_bb(itt);
                                temp_write_basicblock = get_write_basicblock(temp);
                                if (temp_write_basicblock != nullptr) {
                                    control_dependency << "@itt" << std::endl;
                                    return;
                                }
                            }
                            auto b = temp->basicBlock;
                            do {

                                for (auto &i : *(b)) {
                                    if (i.getOpcode() == llvm::Instruction::Call) {
                                        std::string str;
                                        llvm::raw_string_ostream dump(str);
                                        i.print(dump);
//                                        control_dependency << str << std::endl;
                                        if (llvm::isa<llvm::DbgInfoIntrinsic>(i)) {
                                            continue;
                                        }
                                        const llvm::CallInst &cs = llvm::cast<llvm::CallInst>(i);
                                        if (auto tf = cs.getCalledFunction()) {
                                            if (tf->getName().str().find("__sanitizer_cov") != std::string::npos) {
                                            } else if (tf->getName().str().find("__asan") != std::string::npos) {
                                            } else if (tf->getName().str().find("llvm") != std::string::npos) {
                                            } else if (tf->isDeclaration()) {
                                                control_dependency << "@it_isDeclaration" << std::endl;
                                                return;
                                            } else {
                                                call = 1;
                                            }
                                        } else {
                                            std::string str;
                                            llvm::raw_string_ostream dump(str);
                                            cs.getCalledValue()->print(dump);
                                            if (str.find("asm sideeffect") != std::string::npos) {
                                                continue;
                                            }
                                            if (cs.isInlineAsm()) {
                                                std::string str;
                                                llvm::raw_string_ostream dump(str);
                                                cs.getCalledValue()->print(dump);
//                                                control_dependency << str << std::endl;
                                                control_dependency << "@it_isInlineAsm" << std::endl;
                                                return;
                                            }
                                            indirect = 1;
//                                            control_dependency << "@it_indirect" << std::endl;
                                        }
                                    }
                                }

                                b = b->getNextNode();
                            } while (b && !b->hasName());
                        }

                        auto b = db->basicBlock;
                        do {
                            for (auto &i : *(db->basicBlock)) {
                                if (i.getOpcode() == llvm::Instruction::Call) {
                                    std::string str;
                                    llvm::raw_string_ostream dump(str);
                                    i.print(dump);
//                                    control_dependency << str << std::endl;
                                    if (llvm::isa<llvm::DbgInfoIntrinsic>(i)) {
                                        continue;
                                    }
                                    const llvm::CallInst &cs = llvm::cast<llvm::CallInst>(i);
                                    if (auto tf = cs.getCalledFunction()) {
                                        if (tf->getName().str().find("__sanitizer_cov") != std::string::npos) {
                                        } else if (tf->getName().str().find("__asan") != std::string::npos) {
                                        } else if (tf->getName().str().find("llvm") != std::string::npos) {
                                        } else if (tf->isDeclaration()) {
                                            control_dependency << "@isDeclaration" << std::endl;
                                            return;
                                        } else {
                                            call = 1;
                                        }
                                    } else {
                                        std::string str;
                                        llvm::raw_string_ostream dump(str);
                                        cs.getCalledValue()->print(dump);
                                        if (str.find("asm sideeffect") != std::string::npos) {
                                            continue;
                                        }
                                        if (cs.isInlineAsm()) {
                                            std::string str;
                                            llvm::raw_string_ostream dump(str);
                                            cs.getCalledValue()->print(dump);
//                                            control_dependency << str << std::endl;
                                            control_dependency << "@isInlineAsm" << std::endl;
                                            return;
                                        }
                                        indirect = 1;
//                                        control_dependency << "@indirect" << std::endl;
                                    }
                                }
                            }

                            b = b->getNextNode();
                        } while (b && !b->hasName());
                        if (indirect) {
                            control_dependency << "@indirect" << std::endl;
                        } else if (call) {
                            control_dependency << "@call" << std::endl;
                        } else {
                            control_dependency << "@No" << std::endl;
                        }
                        return;
                    } else {
                        goto error;
                    }
                }
            }
            error:
            control_dependency << "@Error" << std::endl;
        };

        if (not_dependency.is_open()) {
            while (getline(not_dependency, Line)) {
                pos_start = 0;
                pos_end = Line.find(delimiter, pos_start + 1);
                if (pos_end - pos_start != 18) {
                    std::cout << "pos_end - pos_start != 18" << std::endl;
                    std::cout << Line << std::endl;
                }
                uncovered_address = std::stoul(Line.substr(pos_start, 18), nullptr, 16);
                pos_start = pos_end;
                pos_end = Line.find(delimiter, pos_start + 1);
                pos_start = pos_end;
                pos_end = Line.find(delimiter, pos_start + 1);
                if (pos_end - pos_start != 19) {
                    std::cout << "pos_end - pos_start != 19" << std::endl;
                    std::cout << Line << std::endl;
                    continue;
                }
                condition_address = std::stoul(Line.substr(pos_start + 1, 18), nullptr, 16);
                check_control_dependency();
            }
        }

        not_dependency.close();
        control_dependency.close();
    }


} /* namespace dra */
