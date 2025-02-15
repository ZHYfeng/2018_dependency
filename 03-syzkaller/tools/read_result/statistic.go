package main

import (
	"fmt"
	pb "github.com/ZHYfeng/Dependency/03-syzkaller/pkg/dra"
	"os"
)

type statistic struct {
	Kind string
	Name string
	tag  []string
	data []float64
}

func (s *statistic) output(path string) {
	fmt.Printf("statistic path : %s\n", path)
	f, _ := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0666)
	_, _ = f.WriteString(fmt.Sprintf("%s", s.Kind))
	for _, v := range s.data {
		_, _ = f.WriteString(fmt.Sprintf("@%.2f", v))
	}
	_, _ = f.WriteString(fmt.Sprintf("\n"))
	_ = f.Close()
}

func average(ss []*statistic) *statistic {

	res := &statistic{
		Kind: "",
		Name: "",
		tag:  []string{},
		data: []float64{},
	}
	if len(ss) > 0 {
		res.Kind = ss[0].Kind
		res.Name = ss[0].Name
		for _, t := range ss[0].tag {
			res.tag = append(res.tag, t)
		}
		res.data = make([]float64, len(res.tag))
	} else {
		return nil
	}
	for _, s := range ss {
		if s.Kind != res.Kind {
			return nil
		} else {
			for i, d := range s.data {
				res.data[i] += d
			}
		}
	}
	for i := range res.data {
		res.data[i] /= float64(len(ss))
	}
	return res
}

func prevalent(r *result) *statistic {
	res := &statistic{
		Kind: "prevalent",
		Name: r.baseName,
		tag: []string{
			"NumberBasicBlockReal",
			"NumberCovered",
			"NumberUncovered",
			"%",
			"NumberUnresolvedConditions",
			"NumberNotDependency",
			"NumberDependency",
			"%",
			"NumberInstructions",
			"NumberInstructionsDominator",
		},
		data: nil,
	}
	res.data = make([]float64, len(res.tag))
	index := 0

	res.data[index+0] = float64(r.statistics.NumberBasicBlockReal)
	fmt.Printf("r.statistics.NumberBasicBlockReal : %d\n", r.statistics.NumberBasicBlockReal)
	res.data[index+1] = float64(r.statistics.NumberBasicBlockCovered)
	res.data[index+2] = res.data[index+0] - res.data[index+1]
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+0]
	}
	index += 4

	res.data[index+0] = float64(len(r.dataDependency.UncoveredAddress))
	res.data[index+1] = 0
	res.data[index+2] = 0
	res.data[index+4] = 0
	res.data[index+5] = 0
	for _, u := range r.dataDependency.UncoveredAddress {
		if u.Kind == pb.UncoveredAddressKind_UncoveredAddressInputRelated {
			res.data[index+1]++
		} else if u.Kind == pb.UncoveredAddressKind_UncoveredAddressDependencyRelated {
			res.data[index+2]++
			res.data[index+4] += float64(u.NumberArriveBasicblocks)
			res.data[index+5] += float64(u.NumberDominatorInstructions)
		}
	}
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+0]
	}
	res.data[index+4] /= res.data[index+1]
	res.data[index+5] /= res.data[index+1]
	index += 6

	return res
}

func writeStatement(r *result) *statistic {
	res := &statistic{
		Kind: "writeStatement",
		Name: r.baseName,
		tag: []string{
			"NumberConditions",
			"NumberWriteStatement",
			"NumberConstant",
			"NumberExpression",
		},
		data: nil,
	}
	res.data = make([]float64, len(res.tag))
	index := 0

	index = 0
	res.data[index+0] = 0
	res.data[index+1] = 0
	res.data[index+2] = 0
	res.data[index+3] = 0
	for _, ua := range r.dataDependency.UncoveredAddress {
		if len(ua.WriteAddress) > 0 {
			res.data[index+0] += 1
			res.data[index+1] += float64(len(ua.WriteAddress))
			for wa := range ua.WriteAddress {
				if ws, ok := r.dataDependency.WriteAddress[wa]; ok {
					if ws.Kind == pb.WriteStatementKind_WriteStatementConstant {
						res.data[index+2]++
					} else if ws.Kind == pb.WriteStatementKind_WriteStatementNonconstant {
						res.data[index+3]++
					} else {

					}
				}
			}
		}
	}

	res.data[index+1] /= res.data[index+0]
	res.data[index+2] /= res.data[index+0]
	res.data[index+3] /= res.data[index+0]
	index += 4

	return res
}

func controlFlow(r *result) *statistic {
	res := &statistic{
		Kind: "controlFlow",
		Name: r.baseName,
		tag: []string{
			"NumberTestCase",
			"NumberCondition",
			"NumberDependency",
			"%",
		},
		data: nil,
	}
	res.data = make([]float64, len(res.tag))
	index := 0

	res.data[index+0] = 0
	res.data[index+1] = 0
	res.data[index+2] = 0
	for _, i := range r.dataDependency.Input {
		if len(i.UncoveredAddress) > 0 {
			res.data[index+0] += 1

			//res.data[index+1] = float64(i.NumberConditions)
			//res.data[index+2] = float64(i.NumberConditionsDependency)

			res.data[index+1] += float64(len(i.UncoveredAddress))
			// fmt.Printf("len(i.UncoveredAddress) : %d\n", len(i.UncoveredAddress))
			for address := range i.UncoveredAddress {
				if ua, ok := r.dataDependency.UncoveredAddress[address]; ok {
					if ua.Kind == pb.UncoveredAddressKind_UncoveredAddressDependencyRelated {
						res.data[index+2] += 1
					}
				}
			}
		}
	}
	res.data[index+1] /= res.data[index+0]
	res.data[index+2] /= res.data[index+0]
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+1]
	}
	index += 4

	return res
}

func unstable(r *result) *statistic {
	res := &statistic{
		Kind: "unstable",
		Name: r.baseName,
		tag: []string{
			"NumberTaskCondition",
			"NumberStable",
			"NumberUnstable",
			"%",
			"NumberTaskWrite",
			"NumberStable",
			"NumberUnstable",
			"%",
			"NumberCombination",
			"NumberStable",
			"NumberUnstable",
			"%",
		},
		data: nil,
	}
	res.data = make([]float64, len(res.tag))
	index := 0

	for _, t := range r.dataRunTime.Tasks.TaskArray {
		for _, ua := range t.UncoveredAddress {
			if ua.CheckCondition {
				res.data[index+1]++
			} else {
				res.data[index+2]++
			}
		}
	}
	res.data[index+0] = res.data[index+1] + res.data[index+2]
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+0]
	}
	index += 4

	for _, t := range r.dataRunTime.Tasks.TaskArray {
		for _, ua := range t.UncoveredAddress {
			if ua.CheckWrite {
				res.data[index+1]++
			} else {
				res.data[index+2]++
			}
		}
	}
	res.data[index+0] = res.data[index+1] + res.data[index+2]
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+0]
	}
	index += 4

	for _, t := range r.dataRunTime.Tasks.TaskArray {
		temp := false
		for _, ua := range t.UncoveredAddress {
			if ua.CheckCondition && ua.CheckWrite {
				temp = true
			}
		}
		if temp == true {
			for _, tr := range t.TaskRunTimeData {
				for _, t := range tr.UncoveredAddress {
					if t.CheckWrite && t.CheckCondition {

					} else {
						res.data[index+2]++
						goto next
					}
				}
			}
			res.data[index+1]++
		next:
		}
	}
	res.data[index+0] = res.data[index+1] + res.data[index+2]
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+0]
	}
	index += 4

	return res
}

func recursive(r *result) *statistic {
	res := &statistic{
		Kind: "recursive",
		Name: r.baseName,
		tag: []string{
			"NumberWriteStatement",
			"NumberCovering",
			"NumberUncovering",
			"%",
			"NumberDependency",
			"%",
		},
		data: nil,
	}
	res.data = make([]float64, len(res.tag))
	index := 0

	for _, wa := range r.dataDependency.WriteAddress {
		res.data[index+0]++
		if len(wa.Input) > 0 {
			res.data[index+1]++
		} else {
			res.data[index+2]++
		}
	}
	if res.data[index+0] == 0 {
		res.data[index+3] = 1
	} else {
		res.data[index+3] = res.data[index+2] / res.data[index+0]
	}
	res.data[index+4] = 0
	if res.data[index+0] == 0 {
		res.data[index+5] = 1
	} else {
		res.data[index+5] = res.data[index+4] / res.data[index+2]
	}
	index += 6

	return res
}
