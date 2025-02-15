// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package report

import (
	"regexp"

	"github.com/ZHYfeng/Dependency/03-syzkaller/sys/targets"
)

type stub struct {
	kernelSrc string
	kernelObj string
	ignores   []*regexp.Regexp
}

func ctorStub(target *targets.Target, kernelSrc, kernelObj string,
	ignores []*regexp.Regexp) (Reporter, []string, error) {
	ctx := &stub{
		kernelSrc: kernelSrc,
		kernelObj: kernelObj,
		ignores:   ignores,
	}
	return ctx, nil, nil
}

func (ctx *stub) ContainsCrash(output []byte) bool {
	panic("not implemented")
}

func (ctx *stub) Parse(output []byte) *Report {
	panic("not implemented")
}

func (ctx *stub) Symbolize(rep *Report) error {
	panic("not implemented")
}
