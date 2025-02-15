// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"github.com/golang/protobuf/proto"
	"math/rand"
	"os"
	"runtime/debug"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/cover"
	pb "github.com/ZHYfeng/Dependency/03-syzkaller/pkg/dra"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/hash"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/ipc"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/log"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/rpctype"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/signal"
	"github.com/ZHYfeng/Dependency/03-syzkaller/prog"
)

const (
	programLength = 30
)

// Proc represents a single fuzzing process (executor).
type Proc struct {
	fuzzer            *Fuzzer
	pid               int
	env               *ipc.Env
	rnd               *rand.Rand
	execOpts          *ipc.ExecOpts
	execOptsCover     *ipc.ExecOpts
	execOptsComps     *ipc.ExecOpts
	execOptsNoCollide *ipc.ExecOpts
}

func newProc(fuzzer *Fuzzer, pid int) (*Proc, error) {
	env, err := ipc.MakeEnv(fuzzer.config, pid)
	if err != nil {
		return nil, err
	}
	rnd := rand.New(rand.NewSource(time.Now().UnixNano() + int64(pid)*1e12))
	execOptsNoCollide := *fuzzer.execOpts
	execOptsNoCollide.Flags &= ^ipc.FlagCollide
	execOptsCover := execOptsNoCollide
	execOptsCover.Flags |= ipc.FlagCollectCover
	execOptsComps := execOptsNoCollide
	execOptsComps.Flags |= ipc.FlagCollectComps
	proc := &Proc{
		fuzzer:            fuzzer,
		pid:               pid,
		env:               env,
		rnd:               rnd,
		execOpts:          fuzzer.execOpts,
		execOptsCover:     &execOptsCover,
		execOptsComps:     &execOptsComps,
		execOptsNoCollide: &execOptsNoCollide,
	}
	return proc, nil
}

func (proc *Proc) loop() {
	generatePeriod := 100
	if proc.fuzzer.config.Flags&ipc.FlagSignal == 0 {
		// If we don't have real coverage signal, generate programs more frequently
		// because fallback signal is weak.
		generatePeriod = 2
	}
	for i := 0; ; i++ {
		ts := time.Now()
		var statName pb.FuzzingStat
		log.Logf(1, "loop : %v", i)

		if item := proc.fuzzer.workQueue.dequeue(); item != nil {
			switch item := item.(type) {
			case *WorkTriage:
				statName = pb.FuzzingStat_StatTriage
				proc.triageInput(item)
			case *WorkCandidate:
				statName = pb.FuzzingStat_StatCandidate
				proc.execute(proc.execOpts, item.p, item.flags, StatCandidate)
			case *WorkDependency:
				statName = pb.FuzzingStat_StatDependency
				proc.dependency(item.task, pb.TaskKind_High)
			case *WorkBoot:
				statName = pb.FuzzingStat_StatDependency
				proc.dependencyBoot(item)
			case *WorkSmash:
				statName = pb.FuzzingStat_StatSmash
				proc.smashInput(item)
			default:
				log.Fatalf("unknown work type: %#v", item)
			}
		} else {
			//r := rand.New(proc.rnd)
			//n := 4
			//m := 2
			//if r.Intn(n) < m {
			item := proc.fuzzer.workQueue.dequeueDependency()
			if item != nil {
				statName = pb.FuzzingStat_StatDependency
				proc.dependency(item.task, pb.TaskKind_Normal)
				//}
			} else {
				ct := proc.fuzzer.choiceTable
				corpus := proc.fuzzer.corpusSnapshot()
				if len(corpus) == 0 || i%generatePeriod == 0 {
					statName = pb.FuzzingStat_StatGenerate
					// Generate a new prog.
					p := proc.fuzzer.target.Generate(proc.rnd, programLength, ct)
					log.Logf(1, "#%v: generated", proc.pid)
					//proc.execute(proc.execOpts, p, ProgNormal, StatGenerate)
					proc.execute(proc.execOptsCover, p, ProgNormal, StatGenerate)
				} else {
					statName = pb.FuzzingStat_StatFuzz
					// Mutate an existing prog.
					log.Logf(1, "#%v: mutated", proc.pid)
					p := corpus[proc.rnd.Intn(len(corpus))].Clone()
					if proc.fuzzer.dManager.DependencyPriority {
						p.MutateD(proc.rnd, programLength, ct, corpus)
					} else {
						p.Mutate(proc.rnd, programLength, ct, corpus)
					}
					proc.execute(proc.execOpts, p, ProgNormal, StatFuzz)
					//info := proc.execute(proc.execOpts, p, ProgNormal, StatFuzz)
					//proc.fuzzer.checkNewCoverage(p, info)
				}
			}
		}

		te := time.Now()
		elapsed := te.Sub(ts)
		s := &pb.Statistic{
			Name:           statName,
			ExecuteNum:     0,
			Time:           elapsed.Seconds(),
			NewTestCaseNum: 0,
			NewAddressNum:  0,
		}
		_, _ = proc.fuzzer.dManager.SendStat(s)
	}
}

func (proc *Proc) triageInput(item *WorkTriage) {

	log.Logf(1, "#%v: triaging type=%x", proc.pid, item.flags)
	prio := signalPrio(item.p, &item.info, item.call)
	inputSignal := signal.FromRaw(item.info.Signal, prio)
	newSignal := proc.fuzzer.corpusSignalDiff(inputSignal)
	if newSignal.Empty() {
		return
	}
	callName := ".extra"
	logCallName := "extra"
	if item.call != -1 {
		callName = item.p.Calls[item.call].Meta.CallName
		logCallName = fmt.Sprintf("call #%v %v", item.call, callName)
	}
	log.Logf(3, "triaging input for %v (new signal=%v)", logCallName, newSignal.Len())
	inputCover := make(cover.Cover)
	const (
		signalRuns       = 3
		minimizeAttempts = 3
	)

	input := &pb.Input{
		Sig:               "",
		Program:           []byte{},
		Call:              map[uint32]*pb.Call{},
		Paths:             []*pb.Paths{},
		Stat:              pb.FuzzingStat_StatTriage,
		ProgramBeforeMini: item.p.Serialize(),
	}

	call := make(map[uint32]uint32)

	// Compute input coverage and non-flaky signal for minimization.
	notexecuted := 0
	for i := 0; i < signalRuns; i++ {
		log.Logf(3, "triaging input signalRuns")
		info := proc.executeRaw(proc.execOptsCover, item.p, StatTriage)
		if !reexecutionSuccess(info, &item.info, item.call) {
			// The call was not executed or failed.
			notexecuted++
			if notexecuted > signalRuns/2+1 {
				return // if happens too often, gi ve up
			}
			continue
		}
		thisSignal, thisCover := getSignalAndCover(item.p, info, item.call)
		newSignal = newSignal.Intersection(thisSignal)
		// Without !minimized check manager starts losing some considerable amount
		// of coverage after each restart. Mechanics of this are not completely clear.
		if newSignal.Empty() && item.flags&ProgMinimized == 0 {
			//if _, _, idx := pb.CheckPath(item.info.Cover, thisCover); idx != 0 {
			//	data := item.p.Serialize()
			//	unstableInput := &pb.UnstableInput{
			//		NewPath: &pb.Path{
			//			Address: make([]uint32, len(item.info.Cover)),
			//		},
			//		UnstablePath: &pb.Path{
			//			Address: make([]uint32, len(item.info.Cover)),
			//		},
			//		Idx:     int32(item.call),
			//		Sig:     hash.Hash(data).String(),
			//		Program: make([]byte, len(data)),
			//	}
			//	copy(unstableInput.NewPath.Address, item.info.Cover)
			//	copy(unstableInput.UnstablePath.Address, thisCover)
			//	copy(unstableInput.Program, data)
			//	proc.fuzzer.dManager.SendUnstableInput(unstableInput)
			//}
			return
		}
		inputCover.Merge(thisCover)

		if pb.StableCoverage {
			if len(call) == 0 {
				for _, a := range thisCover {
					call[a] = 0
				}
			} else {
				temp := make(map[uint32]uint32)
				for _, a := range thisCover {
					if _, ok := call[a]; ok {
						temp[a] = 0
					}
				}
				call = temp
			}
		}

		if pb.CollectPath {
			pps := &pb.Paths{
				Path: map[uint32]*pb.Path{},
			}
			input.Paths = append(input.Paths, pps)
			for i, c := range info.Calls {
				pp := &pb.Path{
					Address: []uint32{},
				}
				pps.Path[uint32(i)] = pp
				for _, a := range c.Cover {
					pp.Address = append(pp.Address, a)
				}
			}
		}
	}

	if item.flags&ProgMinimized == 0 {
		item.p, item.call = prog.Minimize(item.p, item.call, false,
			func(p1 *prog.Prog, call1 int) bool {
				for i := 0; i < minimizeAttempts; i++ {
					log.Logf(3, "minimizeAttempts")
					info := proc.execute(proc.execOptsNoCollide, p1, ProgNormal, StatMinimize)
					if !reexecutionSuccess(info, &item.info, call1) {
						// The call was not executed or failed.
						continue
					}
					thisSignal, _ := getSignalAndCover(p1, info, call1)
					if newSignal.Intersection(thisSignal).Len() == newSignal.Len() {
						return true
					}
				}
				return false
			})
	}

	data := item.p.Serialize()
	sig := hash.Hash(data)

	log.Logf(2, "added new input sig %s for %v to corpus:\n%s", sig.String(), logCallName, data)
	proc.fuzzer.sendInputToManager(rpctype.RPCInput{
		Call:   callName,
		Prog:   data,
		Signal: inputSignal.Serialize(),
		Cover:  inputCover.Serialize(),
	})

	proc.fuzzer.addInputToCorpus(item.p, inputSignal, sig)

	if item.flags&ProgSmashed == 0 {
		proc.fuzzer.workQueue.enqueue(&WorkSmash{item.p, item.call})
	}

	//log.Logf(2, "data :\n%s", data)
	//log.Logf(2, "input.Program :\n%s", input.Program)

	input.Sig = sig.String()
	input.Program = data
	if item.call != -1 {
		cc := &pb.Call{
			Idx:     uint32(item.call),
			Address: make(map[uint32]uint32),
		}
		input.Call[uint32(item.call)] = cc
		if pb.StableCoverage {
			for a := range call {
				cc.Address[a] = 0
			}
		} else {
			for a := range inputCover {
				cc.Address[a] = 0
			}
		}
	}

	for _, c := range item.p.Comments {
		i, ok := pb.FuzzingStat_value[c]
		if ok {
			input.Stat = pb.FuzzingStat(i)
		}
	}
	proc.fuzzer.dManager.SendNewInput(input)
	if pb.CheckCondition {
		proc.checkInput(proto.Clone(input).(*pb.Input))
	}
}

func reexecutionSuccess(info *ipc.ProgInfo, oldInfo *ipc.CallInfo, call int) bool {
	if info == nil || len(info.Calls) == 0 {
		return false
	}
	if call != -1 {
		// Don't minimize calls from successful to unsuccessful.
		// Successful calls are much more valuable.
		if oldInfo.Errno == 0 && info.Calls[call].Errno != 0 {
			return false
		}
		return len(info.Calls[call].Signal) != 0
	}
	return len(info.Extra.Signal) != 0
}

func getSignalAndCover(p *prog.Prog, info *ipc.ProgInfo, call int) (signal.Signal, []uint32) {
	inf := &info.Extra
	if call != -1 {
		inf = &info.Calls[call]
	}
	return signal.FromRaw(inf.Signal, signalPrio(p, inf, call)), inf.Cover
}

func (proc *Proc) smashInput(item *WorkSmash) {
	if proc.fuzzer.faultInjectionEnabled && item.call != -1 {
		proc.failCall(item.p, item.call)
	}
	if proc.fuzzer.comparisonTracingEnabled && item.call != -1 {
		proc.executeHintSeed(item.p, item.call)
	}
	corpus := proc.fuzzer.corpusSnapshot()
	for i := 0; i < 100; i++ {
		p := item.p.Clone()
		if proc.fuzzer.dManager.DependencyPriority {
			p.MutateD(proc.rnd, programLength, proc.fuzzer.choiceTable, corpus)
		} else {
			p.Mutate(proc.rnd, programLength, proc.fuzzer.choiceTable, corpus)
		}
		log.Logf(1, "#%v: smash mutated", proc.pid)
		proc.execute(proc.execOpts, p, ProgNormal, StatSmash)
	}
}

func (proc *Proc) failCall(p *prog.Prog, call int) {
	for nth := 0; nth < 100; nth++ {
		log.Logf(1, "#%v: injecting fault into call %v/%v", proc.pid, call, nth)
		opts := *proc.execOpts
		opts.Flags |= ipc.FlagInjectFault
		opts.FaultCall = call
		opts.FaultNth = nth
		info := proc.executeRaw(&opts, p, StatSmash)
		if info != nil && len(info.Calls) > call && info.Calls[call].Flags&ipc.CallFaultInjected == 0 {
			break
		}
	}
}

func (proc *Proc) executeHintSeed(p *prog.Prog, call int) {
	log.Logf(1, "#%v: collecting comparisons", proc.pid)
	// First execute the original program to dump comparisons from KCOV.
	info := proc.execute(proc.execOptsComps, p, ProgNormal, StatSeed)
	if info == nil {
		return
	}

	// Then mutate the initial program for every match between
	// a syscall argument and a comparison operand.
	// Execute each of such mutants to check if it gives new coverage.
	p.MutateWithHints(call, info.Calls[call].Comps, func(p *prog.Prog) {
		log.Logf(1, "#%v: executing comparison hint", proc.pid)
		proc.execute(proc.execOpts, p, ProgNormal, StatHint)
	})
}

func (proc *Proc) execute(execOpts *ipc.ExecOpts, p *prog.Prog, flags ProgTypes, stat Stat) *ipc.ProgInfo {
	info := proc.executeRaw(execOpts, p, stat)
	calls, extra := proc.fuzzer.checkNewSignal(p, info)
	for _, callIndex := range calls {
		//info := proc.executeRaw(proc.execOptsCover, p, stat)
		p.Comments = []string{pb.FuzzingStat_name[int32(stat)+1]}
		proc.enqueueCallTriage(p, flags, callIndex, info.Calls[callIndex])
	}
	if extra {
		proc.enqueueCallTriage(p, flags, -1, info.Extra)
	}

	if proc.fuzzer.need {
		r := rand.New(proc.rnd)
		n := 100
		m := 1
		if r.Intn(n) < m {
			proc.SendNeedInput(p, info)
		}
	}

	return info
}

func (proc *Proc) enqueueCallTriage(p *prog.Prog, flags ProgTypes, callIndex int, info ipc.CallInfo) {
	// info.Signal points to the output shmem region, detach it before queueing.
	info.Signal = append([]uint32{}, info.Signal...)
	// None of the caller use Cover, so just nil it instead of detaching.
	// Note: triage input uses executeRaw to get coverage.
	info.Cover = nil
	proc.fuzzer.workQueue.enqueue(&WorkTriage{
		p:     p.Clone(),
		call:  callIndex,
		info:  info,
		flags: flags,
	})
}

func (proc *Proc) executeRaw(opts *ipc.ExecOpts, p *prog.Prog, stat Stat) *ipc.ProgInfo {

	if pb.CollectPath {

	} else {
		if opts.Flags&ipc.FlagDedupCover == 0 {
			log.Fatalf("dedup cover is not enabled")
		}
	}

	// Limit concurrency window and do leak checking once in a while.
	ticket := proc.fuzzer.gate.Enter()
	defer proc.fuzzer.gate.Leave(ticket)

	proc.logProgram(opts, p)
	for try := 0; ; try++ {
		atomic.AddUint64(&proc.fuzzer.stats[stat], 1)
		output, info, hanged, err := proc.env.Exec(opts, p)
		if err != nil {
			if try > 10 {
				log.Fatalf("executor %v failed %v times:\n%v", proc.pid, try, err)
			}
			log.Logf(4, "fuzzer detected executor failure='%v', retrying #%d", err, try+1)
			debug.FreeOSMemory()
			time.Sleep(time.Second)
			continue
		}
		log.Logf(2, "result hanged=%v: %s", hanged, output)
		return info
	}
}

func (proc *Proc) logProgram(opts *ipc.ExecOpts, p *prog.Prog) {
	if proc.fuzzer.outputType == OutputNone {
		return
	}

	data := p.Serialize()
	strOpts := ""
	if opts.Flags&ipc.FlagInjectFault != 0 {
		strOpts = fmt.Sprintf(" (fault-call:%v fault-nth:%v)", opts.FaultCall, opts.FaultNth)
	}

	// The following output helps to understand what program crashed kernel.
	// It must not be intermixed.
	switch proc.fuzzer.outputType {
	case OutputStdout:
		now := time.Now()
		proc.fuzzer.logMu.Lock()
		fmt.Printf("%02v:%02v:%02v executing program %v%v:\n%s\n",
			now.Hour(), now.Minute(), now.Second(),
			proc.pid, strOpts, data)
		proc.fuzzer.logMu.Unlock()
	case OutputDmesg:
		fd, err := syscall.Open("/dev/kmsg", syscall.O_WRONLY, 0)
		if err == nil {
			buf := new(bytes.Buffer)
			_, _ = fmt.Fprintf(buf, "syzkaller: executing program %v%v:\n%s\n",
				proc.pid, strOpts, data)
			_, _ = syscall.Write(fd, buf.Bytes())
			_ = syscall.Close(fd)
		}
	case OutputFile:
		f, err := os.Create(fmt.Sprintf("%v-%v.prog", proc.fuzzer.name, proc.pid))
		if err == nil {
			if strOpts != "" {
				_, _ = fmt.Fprintf(f, "#%v\n", strOpts)
			}
			_, _ = f.Write(data)
			_ = f.Close()
		}
	default:
		log.Fatalf("unknown output type: %v", proc.fuzzer.outputType)
	}
}
