// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package netbsd

import (
	"github.com/ZHYfeng/Dependency/03-syzkaller/prog"
	"github.com/ZHYfeng/Dependency/03-syzkaller/sys/targets"
)

func InitTarget(target *prog.Target) {
	arch := &arch{
		unix: targets.MakeUnixSanitizer(target),
	}

	target.MakeMmap = targets.MakePosixMmap(target)
	target.SanitizeCall = arch.unix.SanitizeCall
}

type arch struct {
	unix *targets.UnixSanitizer
}
