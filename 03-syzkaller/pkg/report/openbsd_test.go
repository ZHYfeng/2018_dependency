// Copyright 2018 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package report

import (
	"fmt"
	"testing"

	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/symbolizer"
)

func TestOpenbsdSymbolizeLine(t *testing.T) {
	tests := []struct {
		line   string
		result string
	}{
		// Normal symbolization.
		{
			"closef(ffffffff,ffffffff) at closef+0xaf\n",
			"closef(ffffffff,ffffffff) at closef+0xaf kern_descrip.c:1241\n",
		},
		// Inlined frames.
		{
			"sleep_finish_all(ffffffff,32) at sleep_finish_all+0x22\n",
			"sleep_finish_all(ffffffff,32) at sleep_finish_all+0x22 sleep_finish_timeout kern_synch.c:336 [inline]\n" +
				"sleep_finish_all(ffffffff,32) at sleep_finish_all+0x22 kern_synch.c:157\n",
		},
		// Missing symbol.
		{
			"foo(ffffffff,ffffffff) at foo+0x1e",
			"foo(ffffffff,ffffffff) at foo+0x1e",
		},
		// Witness symbolization.
		{
			"#4  closef+0xaf\n",
			"#4  closef+0xaf kern_descrip.c:1241\n",
		},
		{
			"#10 closef+0xaf\n",
			"#10 closef+0xaf kern_descrip.c:1241\n",
		},
	}
	symbols := map[string][]symbolizer.Symbol{
		"closef": {
			{Addr: 0x815088a0, Size: 0x12f},
		},
		"sleep_finish_all": {
			{Addr: 0x81237520, Size: 0x173},
		},
	}
	symb := func(bin string, pc uint64) ([]symbolizer.Frame, error) {
		if bin != "bsd.gdb" {
			return nil, fmt.Errorf("unknown pc 0x%x", pc)
		}

		switch pc & 0xffffffff {
		case 0x8150894f:
			return []symbolizer.Frame{
				{
					File: "/usr/src/kern_descrip.c",
					Line: 1241,
					Func: "closef",
				},
			}, nil
		case 0x81237542:
			return []symbolizer.Frame{
				{
					Func:   "sleep_finish_timeout",
					File:   "/usr/src/kern_synch.c",
					Line:   336,
					Inline: true,
				},
				{
					Func: "sleep_finish_all",
					File: "/usr/src/kern_synch.c",
					Line: 157,
				},
			}, nil
		default:
			return nil, fmt.Errorf("unknown pc 0x%x", pc)
		}
	}
	obsd := openbsd{
		kernelSrc:    "/usr/src",
		kernelObj:    "/usr/src/sys/arch/amd64/compile/SYZKALLER/obj",
		kernelObject: "bsd.gdb",
		symbols:      symbols,
	}
	for i, test := range tests {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			result := obsd.symbolizeLine(symb, []byte(test.line))
			if test.result != string(result) {
				t.Errorf("want %q\n\t     get %q", test.result, string(result))
			}
		})
	}
}
