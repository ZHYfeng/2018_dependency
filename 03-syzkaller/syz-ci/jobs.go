// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/ZHYfeng/Dependency/03-syzkaller/dashboard/dashapi"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/bisect"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/build"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/instance"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/log"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/mgrconfig"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/osutil"
	"github.com/ZHYfeng/Dependency/03-syzkaller/pkg/vcs"
	"github.com/ZHYfeng/Dependency/03-syzkaller/vm"
)

const (
	commitPollPeriod = time.Hour
)

type JobProcessor struct {
	cfg             *Config
	name            string
	managers        []*Manager
	knownCommits    map[string]bool
	stop            chan struct{}
	shutdownPending chan struct{}
	dash            *dashapi.Dashboard
	syzkallerRepo   string
	syzkallerBranch string
}

func newJobProcessor(cfg *Config, managers []*Manager, stop, shutdownPending chan struct{}) *JobProcessor {
	jp := &JobProcessor{
		cfg:             cfg,
		name:            fmt.Sprintf("%v-job", cfg.Name),
		managers:        managers,
		knownCommits:    make(map[string]bool),
		stop:            stop,
		shutdownPending: shutdownPending,
		syzkallerRepo:   cfg.SyzkallerRepo,
		syzkallerBranch: cfg.SyzkallerBranch,
	}
	if cfg.EnableJobs {
		if cfg.DashboardAddr == "" || cfg.DashboardClient == "" {
			panic("enabled_jobs is set but no dashboard info")
		}
		jp.dash = dashapi.New(cfg.DashboardClient, cfg.DashboardAddr, cfg.DashboardKey)
	}
	return jp
}

func (jp *JobProcessor) loop() {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	var lastCommitPoll time.Time
loop:
	for {
		// Check jp.stop separately first, otherwise if stop signal arrives during a job execution,
		// we can grab the next one with 50% probability.
		select {
		case <-jp.stop:
			break loop
		default:
		}
		select {
		case <-ticker.C:
			if len(kernelBuildSem) != 0 {
				// If normal kernel build is in progress (usually on start), don't query jobs.
				// Otherwise we claim a job, but can't start it for a while.
				continue loop
			}
			if jp.cfg.EnableJobs {
				jp.pollJobs()
			}
			if time.Since(lastCommitPoll) > commitPollPeriod {
				jp.pollCommits()
				lastCommitPoll = time.Now()
			}
		case <-jp.stop:
			break loop
		}
	}
	log.Logf(0, "job loop stopped")
}

func (jp *JobProcessor) pollCommits() {
	for _, mgr := range jp.managers {
		if !mgr.mgrcfg.PollCommits {
			continue
		}
		if err := jp.pollManagerCommits(mgr); err != nil {
			jp.Errorf("failed to poll commits on %v: %v", mgr.name, err)
		}
	}
}

func brokenRepo(url string) bool {
	// TODO(dvyukov): mmots contains weird squashed commits titled "linux-next" or "origin",
	// which contain hundreds of other commits. This makes fix attribution totally broken.
	return strings.Contains(url, "git.cmpxchg.org/linux-mmots")
}

func (jp *JobProcessor) pollManagerCommits(mgr *Manager) error {
	resp, err := mgr.dash.CommitPoll()
	if err != nil {
		return err
	}
	log.Logf(0, "polling commits for %v: repos %v, commits %v", mgr.name, len(resp.Repos), len(resp.Commits))
	if len(resp.Repos) == 0 {
		return fmt.Errorf("no repos")
	}
	commits := make(map[string]*vcs.Commit)
	for i, repo := range resp.Repos {
		if brokenRepo(repo.URL) {
			continue
		}
		if resp.ReportEmail != "" {
			commits1, err := jp.pollRepo(mgr, repo.URL, repo.Branch, resp.ReportEmail)
			if err != nil {
				jp.Errorf("failed to poll %v %v: %v", repo.URL, repo.Branch, err)
				continue
			}
			log.Logf(1, "got %v commits from %v/%v repo", len(commits1), repo.URL, repo.Branch)
			for _, com := range commits1 {
				// Only the "main" repo is the source of true hashes.
				if i != 0 {
					com.Hash = ""
				}
				// Not overwrite existing commits, in particular commit from the main repo with hash.
				if _, ok := commits[com.Title]; !ok && !jp.knownCommits[com.Title] && len(commits) < 100 {
					commits[com.Title] = com
					jp.knownCommits[com.Title] = true
				}
			}
		}
		if i == 0 && len(resp.Commits) != 0 {
			commits1, err := jp.getCommitInfo(mgr, repo.URL, repo.Branch, resp.Commits)
			if err != nil {
				jp.Errorf("failed to poll %v %v: %v", repo.URL, repo.Branch, err)
				continue
			}
			log.Logf(1, "got %v commit infos from %v/%v repo", len(commits1), repo.URL, repo.Branch)
			for _, com := range commits1 {
				// GetCommitByTitle does not accept ReportEmail and does not return tags,
				// so don't replace the existing commit.
				if _, ok := commits[com.Title]; !ok {
					commits[com.Title] = com
				}
			}
		}
	}
	results := make([]dashapi.Commit, 0, len(commits))
	for _, com := range commits {
		results = append(results, dashapi.Commit{
			Hash:   com.Hash,
			Title:  com.Title,
			Author: com.Author,
			BugIDs: com.Tags,
			Date:   com.Date,
		})
	}
	return mgr.dash.UploadCommits(results)
}

func (jp *JobProcessor) pollRepo(mgr *Manager, URL, branch, reportEmail string) ([]*vcs.Commit, error) {
	dir := osutil.Abs(filepath.Join("jobs", mgr.managercfg.TargetOS, "kernel"))
	repo, err := vcs.NewRepo(mgr.managercfg.TargetOS, mgr.managercfg.Type, dir)
	if err != nil {
		return nil, fmt.Errorf("failed to create kernel repo: %v", err)
	}
	if _, err = repo.CheckoutBranch(URL, branch); err != nil {
		return nil, fmt.Errorf("failed to checkout kernel repo %v/%v: %v", URL, branch, err)
	}
	return repo.ExtractFixTagsFromCommits("HEAD", reportEmail)
}

func (jp *JobProcessor) getCommitInfo(mgr *Manager, URL, branch string, commits []string) ([]*vcs.Commit, error) {
	dir := osutil.Abs(filepath.Join("jobs", mgr.managercfg.TargetOS, "kernel"))
	repo, err := vcs.NewRepo(mgr.managercfg.TargetOS, mgr.managercfg.Type, dir)
	if err != nil {
		return nil, fmt.Errorf("failed to create kernel repo: %v", err)
	}
	if _, err = repo.CheckoutBranch(URL, branch); err != nil {
		return nil, fmt.Errorf("failed to checkout kernel repo %v/%v: %v", URL, branch, err)
	}
	results, missing, err := repo.GetCommitsByTitles(commits)
	if err != nil {
		return nil, err
	}
	for _, title := range missing {
		log.Logf(0, "did not find commit %q", title)
	}
	return results, nil
}

func (jp *JobProcessor) pollJobs() {
	var patchTestManagers, bisectManagers []string
	for _, mgr := range jp.managers {
		patchTestManagers = append(patchTestManagers, mgr.name)
		if mgr.mgrcfg.Bisect {
			bisectManagers = append(bisectManagers, mgr.name)
		}
	}
	req, err := jp.dash.JobPoll(&dashapi.JobPollReq{
		PatchTestManagers: patchTestManagers,
		BisectManagers:    bisectManagers,
	})
	if err != nil {
		jp.Errorf("failed to poll jobs: %v", err)
		return
	}
	if req.ID == "" {
		return
	}
	var mgr *Manager
	for _, m := range jp.managers {
		if m.name == req.Manager {
			mgr = m
			break
		}
	}
	if mgr == nil {
		jp.Errorf("got job for unknown manager: %v", req.Manager)
		return
	}
	job := &Job{
		req: req,
		mgr: mgr,
	}
	jp.processJob(job)
}

func (jp *JobProcessor) processJob(job *Job) {
	select {
	case kernelBuildSem <- struct{}{}:
	case <-jp.stop:
		return
	}
	defer func() { <-kernelBuildSem }()

	req := job.req
	log.Logf(0, "starting job %v type %v for manager %v on %v/%v",
		req.ID, req.Type, req.Manager, req.KernelRepo, req.KernelBranch)
	resp := jp.process(job)
	log.Logf(0, "done job %v: commit %v, crash %q, error: %s",
		resp.ID, resp.Build.KernelCommit, resp.CrashTitle, resp.Error)
	select {
	case <-jp.shutdownPending:
		if len(resp.Error) != 0 {
			// Ctrl+C can kill a child process which will cause an error.
			log.Logf(0, "ignoring error: shutdown pending")
			return
		}
	default:
	}
	if err := jp.dash.JobDone(resp); err != nil {
		jp.Errorf("failed to mark job as done: %v", err)
		return
	}
}

type Job struct {
	req  *dashapi.JobPollResp
	resp *dashapi.JobDoneReq
	mgr  *Manager
}

func (jp *JobProcessor) process(job *Job) *dashapi.JobDoneReq {
	req, mgr := job.req, job.mgr

	dir := osutil.Abs(filepath.Join("jobs", mgr.managercfg.TargetOS))
	mgrcfg := new(mgrconfig.Config)
	*mgrcfg = *mgr.managercfg
	mgrcfg.Workdir = filepath.Join(dir, "workdir")
	mgrcfg.KernelSrc = filepath.Join(dir, "kernel")
	mgrcfg.Syzkaller = filepath.Join(dir, "gopath", "src", "github.com", "google", "syzkaller")
	os.RemoveAll(mgrcfg.Workdir)
	defer os.RemoveAll(mgrcfg.Workdir)

	resp := &dashapi.JobDoneReq{
		ID: req.ID,
		Build: dashapi.Build{
			Manager:         mgr.name,
			ID:              req.ID,
			OS:              mgr.managercfg.TargetOS,
			Arch:            mgr.managercfg.TargetArch,
			VMArch:          mgr.managercfg.TargetVMArch,
			SyzkallerCommit: req.SyzkallerCommit,
		},
	}
	job.resp = resp
	switch req.Type {
	case dashapi.JobTestPatch:
		mgrcfg.Name += "-test-job"
		resp.Build.CompilerID = mgr.compilerID
		resp.Build.KernelRepo = req.KernelRepo
		resp.Build.KernelBranch = req.KernelBranch
		resp.Build.KernelCommit = "[unknown]"
	case dashapi.JobBisectCause, dashapi.JobBisectFix:
		mgrcfg.Name += "-bisect-job"
		resp.Build.KernelRepo = mgr.mgrcfg.Repo
		resp.Build.KernelBranch = mgr.mgrcfg.Branch
		resp.Build.KernelCommit = req.KernelCommit
		resp.Build.KernelCommitTitle = req.KernelCommitTitle
		resp.Build.KernelCommitDate = req.KernelCommitDate
		resp.Build.KernelConfig = req.KernelConfig
	default:
		err := fmt.Errorf("bad job type %v", req.Type)
		job.resp.Error = []byte(err.Error())
		jp.Errorf("%s", err)
		return job.resp
	}

	required := []struct {
		name string
		ok   bool
	}{
		{"kernel repository", req.KernelRepo != "" || req.Type != dashapi.JobTestPatch},
		{"kernel branch", req.KernelBranch != "" || req.Type != dashapi.JobTestPatch},
		{"kernel config", len(req.KernelConfig) != 0},
		{"syzkaller commit", req.SyzkallerCommit != ""},
		{"reproducer options", len(req.ReproOpts) != 0},
		{"reproducer program", len(req.ReproSyz) != 0},
	}
	for _, req := range required {
		if !req.ok {
			job.resp.Error = []byte(req.name + " is empty")
			jp.Errorf("%s", job.resp.Error)
			return job.resp
		}
	}
	if typ := mgr.managercfg.Type; !vm.AllowsOvercommit(typ) {
		job.resp.Error = []byte(fmt.Sprintf("testing is not yet supported for %v machine type.", typ))
		jp.Errorf("%s", job.resp.Error)
		return job.resp
	}

	var err error
	switch req.Type {
	case dashapi.JobTestPatch:
		mgrcfg.Name += "-test-job"
		err = jp.testPatch(job, mgrcfg)
	case dashapi.JobBisectCause, dashapi.JobBisectFix:
		mgrcfg.Name += "-bisect-job"
		err = jp.bisect(job, mgrcfg)
	}
	if err != nil {
		job.resp.Error = []byte(err.Error())
	}
	return job.resp
}

func (jp *JobProcessor) bisect(job *Job, mgrcfg *mgrconfig.Config) error {
	req, resp, mgr := job.req, job.resp, job.mgr

	trace := new(bytes.Buffer)
	cfg := &bisect.Config{
		Trace:    io.MultiWriter(trace, log.VerboseWriter(3)),
		DebugDir: osutil.Abs(filepath.Join("jobs", "debug", strings.Replace(req.ID, "|", "_", -1))),
		Fix:      req.Type == dashapi.JobBisectFix,
		BinDir:   jp.cfg.BisectBinDir,
		Kernel: bisect.KernelConfig{
			Repo:      mgr.mgrcfg.Repo,
			Branch:    mgr.mgrcfg.Branch,
			Commit:    req.KernelCommit,
			Cmdline:   mgr.mgrcfg.KernelCmdline,
			Sysctl:    mgr.mgrcfg.KernelSysctl,
			Config:    req.KernelConfig,
			Userspace: mgr.mgrcfg.Userspace,
		},
		Syzkaller: bisect.SyzkallerConfig{
			Repo:   jp.syzkallerRepo,
			Commit: req.SyzkallerCommit,
		},
		Repro: bisect.ReproConfig{
			Opts: req.ReproOpts,
			Syz:  req.ReproSyz,
			C:    req.ReproC,
		},
		Manager: *mgrcfg,
	}

	commits, rep, err := bisect.Run(cfg)
	resp.Log = trace.Bytes()
	if err != nil {
		return err
	}
	for _, com := range commits {
		resp.Commits = append(resp.Commits, dashapi.Commit{
			Hash:       com.Hash,
			Title:      com.Title,
			Author:     com.Author,
			AuthorName: com.AuthorName,
			CC:         com.CC,
			Date:       com.Date,
		})
	}
	if rep != nil {
		resp.CrashTitle = rep.Title
		resp.CrashReport = rep.Report
		resp.CrashLog = rep.Output
		if len(resp.Commits) != 0 {
			resp.Commits[0].CC = append(resp.Commits[0].CC, rep.Maintainers...)
		}
	}
	return nil
}

func (jp *JobProcessor) testPatch(job *Job, mgrcfg *mgrconfig.Config) error {
	req, resp, mgr := job.req, job.resp, job.mgr

	env, err := instance.NewEnv(mgrcfg)
	if err != nil {
		return err
	}
	log.Logf(0, "job: building syzkaller on %v...", req.SyzkallerCommit)
	if err := env.BuildSyzkaller(jp.syzkallerRepo, req.SyzkallerCommit); err != nil {
		return err
	}

	log.Logf(0, "job: fetching kernel...")
	repo, err := vcs.NewRepo(mgrcfg.TargetOS, mgrcfg.Type, mgrcfg.KernelSrc)
	if err != nil {
		return fmt.Errorf("failed to create kernel repo: %v", err)
	}
	var kernelCommit *vcs.Commit
	if vcs.CheckCommitHash(req.KernelBranch) {
		kernelCommit, err = repo.CheckoutCommit(req.KernelRepo, req.KernelBranch)
		if err != nil {
			return fmt.Errorf("failed to checkout kernel repo %v on commit %v: %v",
				req.KernelRepo, req.KernelBranch, err)
		}
		resp.Build.KernelBranch = ""
	} else {
		kernelCommit, err = repo.CheckoutBranch(req.KernelRepo, req.KernelBranch)
		if err != nil {
			return fmt.Errorf("failed to checkout kernel repo %v/%v: %v",
				req.KernelRepo, req.KernelBranch, err)
		}
	}
	resp.Build.KernelCommit = kernelCommit.Hash
	resp.Build.KernelCommitTitle = kernelCommit.Title
	resp.Build.KernelCommitDate = kernelCommit.Date

	if err := build.Clean(mgrcfg.TargetOS, mgrcfg.TargetVMArch, mgrcfg.Type, mgrcfg.KernelSrc); err != nil {
		return fmt.Errorf("kernel clean failed: %v", err)
	}
	if len(req.Patch) != 0 {
		if err := vcs.Patch(mgrcfg.KernelSrc, req.Patch); err != nil {
			return err
		}
	}

	log.Logf(0, "job: building kernel...")
	if err := env.BuildKernel(mgr.mgrcfg.Compiler, mgr.mgrcfg.Userspace, mgr.mgrcfg.KernelCmdline,
		mgr.mgrcfg.KernelSysctl, req.KernelConfig); err != nil {
		return err
	}
	resp.Build.KernelConfig, err = ioutil.ReadFile(filepath.Join(mgrcfg.KernelSrc, ".config"))
	if err != nil {
		return fmt.Errorf("failed to read config file: %v", err)
	}

	log.Logf(0, "job: testing...")
	results, err := env.Test(3, req.ReproSyz, req.ReproOpts, req.ReproC)
	if err != nil {
		return err
	}
	// We can have transient errors and other errors of different types.
	// We need to avoid reporting transient "failed to boot" or "failed to copy binary" errors.
	// If any of the instances crash during testing, we report this with the highest priority.
	// Then if any of the runs succeed, we report that (to avoid transient errors).
	// If all instances failed to boot, then we report one of these errors.
	anySuccess := false
	var anyErr, testErr error
	for _, res := range results {
		if res == nil {
			anySuccess = true
			continue
		}
		anyErr = res
		switch err := res.(type) {
		case *instance.TestError:
			// We should not put rep into resp.CrashTitle/CrashReport,
			// because that will be treated as patch not fixing the bug.
			if rep := err.Report; rep != nil {
				testErr = fmt.Errorf("%v\n\n%s\n\n%s", rep.Title, rep.Report, rep.Output)
			} else {
				testErr = fmt.Errorf("%v\n\n%s", err.Title, err.Output)
			}
		case *instance.CrashError:
			resp.CrashTitle = err.Report.Title
			resp.CrashReport = err.Report.Report
			resp.CrashLog = err.Report.Output
			return nil
		}
	}
	if anySuccess {
		return nil
	}
	if testErr != nil {
		return testErr
	}
	return anyErr
}

// Errorf logs non-fatal error and sends it to dashboard.
func (jp *JobProcessor) Errorf(msg string, args ...interface{}) {
	log.Logf(0, "job: "+msg, args...)
	if jp.dash != nil {
		jp.dash.LogError(jp.name, msg, args...)
	}
}
