// Copyright 2016 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog_test

import (
	"testing"

	. "github.com/ZHYfeng/Dependency/03-syzkaller/prog"
	_ "github.com/ZHYfeng/Dependency/03-syzkaller/sys"
)

func TestChecksumCalcRandom(t *testing.T) {
	target, rs, iters := InitTest(t)
	for i := 0; i < iters; i++ {
		p := target.Generate(rs, 10, nil)
		for _, call := range p.Calls {
			CalcChecksumsCall(call)
		}
		for try := 0; try <= 10; try++ {
			p.Mutate(rs, 10, nil, nil)
			for _, call := range p.Calls {
				CalcChecksumsCall(call)
			}
		}
	}
}
