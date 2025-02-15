package dra

import (
	"os"
	"path/filepath"
)

// useful const
const (
	ClientMaxReceiveMessageSize = 1024 * 1024 * 100

	TimeStart = 10800
	//TimeStart       = 0
	TimeNew         = 3600
	TimeBoot        = 3600
	TimeWriteToDisk = 3600
	Exit            = false
	TimeExit        = 3600 * 24

	DebugLevel = 2

	TaskQueueNumber     = 4
	TaskCountLimitation = 2
	TaskCountBase       = 1

	NeedBoot  = false
	NeedInput = false

	TaskBoot = false

	// collect original path
	CollectPath = false
	// if the path is unstable, collect all of them
	CollectUnstable = false
	// collect coverage by intersection instead of union.
	StableCoverage = true
	// check uncovered Condition address in syz-fuzzer once find new test case
	CheckCondition = true
)

var pathProject = os.Getenv("PATH_PROJECT")
var pathWorkdir = filepath.Join(pathProject, "workdir")

var pathLinux = filepath.Join(pathWorkdir, "13-linux-clang-np")
var FileVmlinuxObjdump = filepath.Join(pathLinux, "vmlinux.objdump")

const (
	NameDevice         = "dev_"
	NameBase           = "base"
	NameWithDra        = "01-result-with-dra"
	NameWithoutDra     = "02-result-without-dra"
	NameData           = "data.txt"
	NameDataDependency = "dataDependency.bin"
	NameDataResult     = "dataResult.bin"
	NameDataRunTime    = "dataRunTime.bin"
	NameStatistics     = "statistics.bin"
	NameUnstable       = "unstable.bin"

	NameDriver    = "built-in"
	FileAsm       = NameDriver + ".s"
	FileBc        = NameDriver + ".bc"
	FileDRAConfig = "dra.json"

	NameResultUnstable = "unstable.txt"
)
