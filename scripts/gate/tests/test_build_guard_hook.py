"""Tests for scripts/claude_hooks/build_guard.py, the Claude Code PreToolUse hook
that refuses build commands which would skip the machine-wide build jobserver.

Why it exists: 2026-09-30 13:16, a coordinator's lane briefs said
`cmake --build build --target bezel_tests_sema -j` (unlimited `make -j`,
outside dev_heavy.sh). Lanes copied it, 499 clang ran, and the OOM killer took
three Claude sessions. scripts/build_token.sh bounds such a build anyway; this
hook stops it at the tool call and at the brief that would spread it.

Pin groups:
  1. Denied: the incident command, a build hidden behind wrappers (nohup,
     setsid, timeout, env, systemd-run, keywords), inside `bash -c`, `$( )`,
     `eval`, a heredoc fed to a shell; make/ninja in a CMake build dir;
     `dev_heavy.sh build bash -c ...` and `dev_heavy.sh probe cmake --build`;
     jobserver tampering (BEZEL_JOBSERVER=off, MAKEFLAGS=, --jobserver-auth).
  2. Allowed: the supported paths (dev_heavy.sh build cmake ..., agent_check),
     configure, dry runs, make outside a build dir, and text that only
     mentions a build (grep, echo, commit messages, a heredoc into a file).
  3. Brief lint: an Agent prompt or a brief file that instructs a bare build is
     denied; a line that forbids it is not.
  4. Plumbing: out-of-root calls are ignored; bad input exits 0 silently; the
     deny JSON has the shape Claude Code expects; denials are logged.
Every case runs the real script as a subprocess, the way Claude Code runs it.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = os.path.abspath(os.path.join(HERE, "..", "..", "claude_hooks", "build_guard.py"))
sys.path.insert(0, os.path.dirname(HOOK))

import build_guard as bg  # noqa: E402

INCIDENT = "cmake --build build --target bezel_tests_sema -j"


class HookTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="build-guard-test-")
        self.root = os.path.realpath(self._tmp.name)
        self.repo = os.path.join(self.root, "repo")
        self.build = os.path.join(self.repo, "build")
        os.makedirs(os.path.join(self.repo, "scripts"))
        os.makedirs(os.path.join(self.build, "tests"))
        with open(os.path.join(self.repo, "scripts", "dev_heavy.sh"), "w") as f:
            f.write("#!/bin/sh\n")
        with open(os.path.join(self.build, "CMakeCache.txt"), "w") as f:
            f.write(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.repo}\n")
        self.plain = os.path.join(self.root, "plain")
        os.makedirs(self.plain)
        self.outside = os.path.join(self.root, "elsewhere")
        os.makedirs(self.outside)
        self.log = os.path.join(self.root, "denials.log")

    def tearDown(self):
        self._tmp.cleanup()

    def run_hook(self, event, raw=None):
        env = dict(os.environ)
        p = subprocess.run(
            [sys.executable, HOOK, "--root", self.repo, "--root", self.plain,
             "--deny-log", self.log],
            input=raw if raw is not None else json.dumps(event),
            capture_output=True, text=True, timeout=30, env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def decision(self, event):
        out = self.run_hook(event).stdout
        if not out.strip():
            return None
        doc = json.loads(out)
        hso = doc["hookSpecificOutput"]
        self.assertEqual(hso["hookEventName"], "PreToolUse")
        self.assertEqual(hso["permissionDecision"], "deny")
        return hso["permissionDecisionReason"]

    def bash(self, command, cwd=None):
        return self.decision({"tool_name": "Bash", "cwd": cwd or self.repo,
                              "session_id": "s1", "tool_input": {"command": command}})

    def assertDenied(self, command, cwd=None, needle=None):
        reason = self.bash(command, cwd)
        self.assertIsNotNone(reason, f"not denied: {command!r}")
        if needle:
            self.assertIn(needle, reason)
        return reason

    def assertAllowed(self, command, cwd=None):
        reason = self.bash(command, cwd)
        self.assertIsNone(reason, f"denied: {command!r}\n{reason}")


class DeniedBuildsTest(HookTestBase):
    def test_incident_command_denied_with_agent_check_replacement(self):
        reason = self.assertDenied(INCIDENT)
        self.assertIn("scripts/agent_check.sh --area sema --build-only", reason)

    def test_plain_cmake_build_denied_with_dev_heavy_replacement(self):
        reason = self.assertDenied("cmake --build build --target bezelc -j16")
        self.assertIn(f"{self.repo}/scripts/dev_heavy.sh build cmake --build build "
                      "--target bezelc", reason)
        self.assertNotIn("-j16", reason.split("Run instead:")[1].splitlines()[0])

    def test_all_test_binaries_target(self):
        self.assertIn("scripts/agent_check.sh --build-only",
                      self.assertDenied("cmake --build build --target bezel_tests"))

    def test_wrappers(self):
        for cmd in (f"nohup {INCIDENT} > log 2>&1 &",
                    f"setsid nohup {INCIDENT} > log 2>&1 < /dev/null &",
                    f"timeout 600 {INCIDENT}",
                    f"timeout -k 5 600 {INCIDENT}",
                    f"env -u FOO CCACHE_DISABLE=1 {INCIDENT}",
                    f"CCACHE_DISABLE=1 {INCIDENT}",
                    f"systemd-run --user --scope -p MemoryMax=8G {INCIDENT}",
                    f"nice -n 19 ionice -c3 {INCIDENT}",
                    f"time {INCIDENT}",
                    f"/usr/bin/cmake --build build",
                    f"if true; then {INCIDENT}; fi",
                    f"for i in 1 2; do {INCIDENT}; done",
                    f"{{ {INCIDENT}; }}",
                    f"cd {self.repo} && {INCIDENT} 2>&1 | tail -5"):
            with self.subTest(cmd=cmd):
                self.assertDenied(cmd)

    def test_nested_shells(self):
        for cmd in (f"bash -c '{INCIDENT}'",
                    f"bash -lc \"cd {self.repo} && {INCIDENT}\"",
                    f"sh -c 'sh -c \"{INCIDENT}\"'",
                    f"echo $({INCIDENT})",
                    f"echo \"`{INCIDENT}`\"",
                    f"eval '{INCIDENT}'",
                    f"x=$(cat <(cmake --build build))",
                    f"bash <<'EOF'\n{INCIDENT}\nEOF",
                    f"bash -s <<EOF\ncd build\nmake -j8\nEOF"):
            with self.subTest(cmd=cmd):
                self.assertDenied(cmd)

    def test_line_continuation(self):
        self.assertDenied("cmake \\\n  --build build \\\n  --target bezel_tests_codegen")

    def test_make_and_ninja_in_build_dir(self):
        for cmd in ("cd build && make -j8",
                    "make -C build -j",
                    f"make -C {self.build}/tests",
                    "make -f build/Makefile",
                    "ninja -C build bezelc",
                    "gmake --directory=build"):
            with self.subTest(cmd=cmd):
                self.assertDenied(cmd)
        reason = self.assertDenied("make -C build -j64 bezelc")
        self.assertIn(f"{self.repo}/scripts/dev_heavy.sh build make -C build bezelc", reason)

    def test_make_cwd_is_build_dir(self):
        self.assertDenied("make -j", cwd=self.build)
        self.assertDenied("make", cwd=os.path.join(self.build, "tests"))

    def test_dev_heavy_build_with_wrapper_payload(self):
        for cmd in (f"scripts/dev_heavy.sh build bash -c '{INCIDENT}'",
                    "scripts/dev_heavy.sh build sh -c 'cd build && make -j'",
                    "scripts/dev_heavy.sh build env FOO=1 cmake --build build"):
            with self.subTest(cmd=cmd):
                self.assertDenied(cmd, needle="right after `build`")

    def test_dev_heavy_other_kinds_with_build(self):
        for kind in ("probe", "suite"):
            with self.subTest(kind=kind):
                self.assertDenied(f"scripts/dev_heavy.sh {kind} cmake --build build",
                                  needle="does not join the jobserver")

    def test_jobserver_tampering(self):
        for cmd in ("BEZEL_JOBSERVER=off scripts/dev_heavy.sh build cmake --build build",
                    "export BEZEL_JOBSERVER=off",
                    "BEZEL_JOBSERVER=0 true",
                    "export MAKEFLAGS=-j64",
                    "MAKEFLAGS='-j' scripts/agent_check.sh --area sema x",
                    "declare -x GNUMAKEFLAGS=-j",
                    "BEZEL_BUILD_TOKEN_SLOTS=999 make -C /x",
                    "make --jobserver-auth=fifo:/tmp/x"):
            with self.subTest(cmd=cmd):
                self.assertDenied(cmd)


class AllowedCommandsTest(HookTestBase):
    def test_supported_paths(self):
        for cmd in ("scripts/dev_heavy.sh build cmake --build build --target bezelc",
                    "scripts/dev_heavy.sh build cmake --build build -j16",
                    f"{self.repo}/scripts/dev_heavy.sh build make -C build",
                    "scripts/dev_heavy.sh build ninja -C build",
                    "nohup scripts/dev_heavy.sh build cmake --build build > log 2>&1 &",
                    "scripts/agent_check.sh --area sema Foo",
                    "scripts/agent_check.sh --area codegen --build-only",
                    "BEZEL_JOBSERVER=on scripts/dev_heavy.sh build cmake --build build",
                    "scripts/dev_heavy.sh probe build/bezelc x.bzl",
                    "scripts/dev_heavy.sh suite build/tests/bezel_tests_sema"):
            with self.subTest(cmd=cmd):
                self.assertAllowed(cmd)

    def test_configure_and_dry_runs(self):
        for cmd in ("cmake -S . -B build -G Ninja",
                    "cmake -E echo hi",
                    "cmake build",
                    "cmake --build build --target bezel_tests_module -- -n",
                    "cmake --build build --target help",
                    "cmake --build build --target clean",
                    "make -C build -n bezelc",
                    "make -C build help",
                    "ninja -C build -t targets",
                    "ninja -C build -n"):
            with self.subTest(cmd=cmd):
                self.assertAllowed(cmd)

    def test_cmake_target_short_flag_is_not_ninja_tool(self):
        # cmake's -t is --target; only ninja's -t selects a tool.
        self.assertDenied("cmake --build build -t bezel_tests_sema")

    def test_make_outside_build_dir(self):
        self.assertAllowed("make -j8", cwd=self.plain)
        self.assertAllowed(f"make -C {self.plain} -j")

    def test_mentions_are_not_runs(self):
        for cmd in ('grep -rn "cmake --build" docs',
                    "rg 'make -j' scripts",
                    "echo cmake --build build -j",
                    'git commit -m "never run cmake --build -j"',
                    "git commit -m \"$(cat <<'EOF'\nDo not run cmake --build build -j.\n"
                    "MAKEFLAGS stays unset.\nEOF\n)\"",
                    f"cat > brief.md <<'EOF'\n{INCIDENT}\nEOF",
                    "printf '%s\\n' 'cd build && make -j8'",
                    "# cmake --build build\ntrue",
                    "python3 -c 'print(\"cmake --build build\")'"):
            with self.subTest(cmd=cmd):
                self.assertAllowed(cmd)

    def test_outside_roots_is_ignored(self):
        self.assertAllowed("cmake --build /other/build -j", cwd=self.outside)
        self.assertAllowed("export MAKEFLAGS=-j64", cwd=self.outside)

    def test_command_naming_a_root_is_checked_from_outside(self):
        self.assertDenied(f"cd {self.repo} && {INCIDENT}", cwd=self.outside)


class BriefLintTest(HookTestBase):
    def agent(self, prompt, tool="Agent"):
        key = "message" if tool == "SendMessage" else "prompt"
        return self.decision({"tool_name": tool, "cwd": self.repo,
                              "tool_input": {key: prompt, "description": "x"}})

    def test_prompt_with_bare_build_denied(self):
        for text in (f"Build with `{INCIDENT}` then run the filter.",
                     "Step 3: cd build && make -j8",
                     "Run BEZEL_JOBSERVER=off scripts/agent_check.sh --area sema X"):
            for tool in ("Agent", "Task", "SendMessage"):
                with self.subTest(text=text, tool=tool):
                    self.assertIsNotNone(self.agent(text, tool))

    def test_prompt_that_forbids_is_allowed(self):
        for text in (f"Never run `{INCIDENT}`; use scripts/agent_check.sh.",
                     "Do not use make -j in build/.",
                     "Build: scripts/dev_heavy.sh build cmake --build build --target bezelc",
                     "Build: scripts/agent_check.sh --area sema --build-only",
                     "Fix the parser; no build instructions here."):
            with self.subTest(text=text):
                self.assertIsNone(self.agent(text))

    def test_brief_file_write_and_edit(self):
        path = os.path.join(self.repo, "lane-brief-5100.md")
        self.assertIsNotNone(self.decision({"tool_name": "Write", "cwd": self.repo,
                                            "tool_input": {"file_path": path,
                                                           "content": f"1. {INCIDENT}\n"}}))
        self.assertIsNotNone(self.decision({"tool_name": "Edit", "cwd": self.repo,
                                            "tool_input": {"file_path": path, "old_string": "a",
                                                           "new_string": f"1. {INCIDENT}\n"}}))

    def test_non_brief_file_is_not_linted(self):
        path = os.path.join(self.repo, "docs", "ops", "coordinator-runbook.md")
        self.assertIsNone(self.decision({"tool_name": "Write", "cwd": self.repo,
                                         "tool_input": {"file_path": path,
                                                        "content": f"{INCIDENT}\n"}}))


class KillTest(HookTestBase):
    """2026-10-01: a lane killed a stray run with a by-name kill and took other
    lanes' test processes with it. By-name/pattern kills are denied; kills by
    PID, by parent/pidfile/session, or by one lane's own path are allowed."""

    def agent(self, prompt):
        return self.decision({"tool_name": "Agent", "cwd": self.repo,
                              "tool_input": {"prompt": prompt, "description": "x"}})

    def test_by_name_kills_denied(self):
        wt = f"{self.plain}/worktrees/wt1"
        for cmd in ("pkill clang",
                    "pkill -f bezel_tests",
                    "pkill -9 -f cc1plus",
                    "pkill -KILL -f 'build/tests/bezel_tests_codegen'",
                    "pkill -u stepan",
                    "pkill -f /usr/bin/clang",
                    f"pkill -f {self.plain}/",            # the whole root
                    f"pkill -f {self.plain}/worktrees/",  # every lane
                    f"pkill -f {wt}",                     # also matches wt1-rv
                    f"pkill -f {self.plain}/aot-harness/bezel_",  # shared dir
                    f"pkill -f '{self.plain}/worktrees/wt.*/'",  # regex in the lane level
                    f"pkill -f 'clang|{wt}/'",            # alternation drops the path
                    f"pkill -f '(x)?{wt}/'",              # group before the path
                    f"pkill -f '{wt}/?'",                 # closing `/` made optional
                    f"pkill -f 'a24-wt-5173-rv.*bezel_tests_codegen'",  # not a path
                    "pkill -f 'agent_check.sh.*sema'",
                    "pkill -9 -f 'cmake --build.*bezel_tests'",
                    "pgrep -f 'a24-wt-5191/build' | xargs -r kill",  # relative, not a lane path
                    "pgrep -f 'worktrees/wt.*/' | xargs kill",       # regex in the lane level
                    "pkill -f 'worktrees/r20-coord'",                 # lane level not closed
                    "left=$(ps -eo pid= ); kill $left",
                    f"pgrep -f '{wt}' | xargs kill",      # lane level not closed
                    f"ps aux | grep bezel_tests_codegen | grep {wt}/ ; pgrep make | xargs kill",
                    "kill -9 $(pgrep -f bezel_tests_codegen) 2>/dev/null; kill -0 $(pgrep x)",
                    "killall clang++",
                    "killall -9 make",
                    "kill $(pgrep clang)",
                    "kill -9 $(pgrep -f bezel_tests)",
                    "kill `pidof cc1plus`",
                    "kill -TERM \"$(ps -eo pid,cmd | grep cmake | awk '{print $1}')\"",
                    "pgrep -f cmake | xargs kill",
                    "pgrep cc1plus | xargs -r kill -9",
                    "ps aux | grep clang | awk '{print $2}' | xargs kill",
                    "for p in $(pgrep clang); do kill $p; done",
                    "pids=$(pgrep -f bezel); kill -9 $pids",
                    "P=`pgrep make`; kill \"${P}\"",
                    "kill -9 -1",
                    "kill -- -1",
                    "bash -c 'pkill clang'",
                    "nohup pkill -f x > /dev/null 2>&1 &",
                    "timeout 5 killall make",
                    "sudo pkill cmake",
                    f"cd {self.repo} && pkill -f bezel_tests_sema"):
            with self.subTest(cmd=cmd):
                self.assertDenied(cmd, needle="selects processes by name or pattern")

    def test_scoped_kills_allowed(self):
        wt = f"{self.plain}/worktrees/wt1"
        for cmd in ("kill 12345",
                    "kill -9 12345 12346",
                    "kill -TERM 123",
                    "kill -s KILL 123",
                    "kill -0 2586497",
                    "kill -0 $PID 2>/dev/null && echo alive",
                    # liveness probes send no signal
                    "while kill -0 $(pgrep -f 'agent_check.sh --area sema' | head -1) 2>/dev/null; "
                    "do sleep 20; done",
                    "P=$(ps -eo pid,args | awk '/codegen_half.sh/ {print $1; exit}'); "
                    "kill -0 $P && echo running",
                    "kill -s 0 $(pidof make)",
                    # PIDs listed by a lane-scoped finder
                    f"P=$(pgrep -f '^{wt}/build/tests/bezel_tests_codegen'); [ -n \"$P\" ] && kill $P",
                    f"pgrep -f '{wt}/build/tests/bezel_tests_codegen' | xargs kill 2>/dev/null",
                    f"ps aux | grep '{wt}/build/' | grep -v grep | awk '{{print $2}}' | xargs -r kill -9",
                    f"kill -9 $(pgrep -f {wt}/build/)",
                    f"for p in $(pgrep -P 1234); do kill $p; done",
                    "cat /var/tmp/bezel-scratch/r25/x.pids | xargs kill",
                    # a by-name listing after the kill cannot feed it
                    f"pgrep -f '{wt}/build/tests/x' | xargs kill; sleep 2; ps aux | grep x | wc -l",
                    # `ps -p <PIDs>` lists only the PIDs it was given
                    "left=$(ps -o pid= -p 2243003,2243001); [ -n \"$left\" ] && kill -9 $left",
                    "kill -TERM -- -$(ps -o pgid= -p 974410 | tr -d ' ')",
                    # a relative `worktrees/<wt>/` names one lane
                    "pgrep -f 'worktrees/r20-coord/build' | xargs -r kill -TERM",
                    "pkill -f lane-builds/r20-l1-e031b0e1/",
                    f"pkill -f 'codegen_half.sh {wt}/'",  # plain text before the lane path
                    "kill %1",
                    "kill $!",
                    "kill \"$pid\"",
                    "kill $(cat /var/tmp/bezel-scratch/r25/x.pid)",
                    "kill -l",
                    "pgrep -af clang",
                    "ps -eo pid,cmd | grep cmake",
                    "pkill -P 12345",
                    "pkill -TERM -P 12345",
                    "pkill --parent=12345",
                    "pkill -F /var/tmp/bezel-scratch/r25/run.pid",
                    "pkill -s 4321",
                    f"pkill -f {wt}/",
                    f"pkill -f '{wt}/.*cmake'",
                    f"pkill -9 -f {wt}/build/tests/bezel_tests_codegen",
                    f"pkill -f \"$(readlink -f {wt}/build)/\"",
                    f"pkill -f {self.repo}/.claude/worktrees/x/build/",
                    "echo 'pkill clang'",
                    'git commit -m "never pkill by name"',
                    "grep -rn 'pkill' scripts",
                    "# pkill clang\ntrue"):
            with self.subTest(cmd=cmd):
                self.assertAllowed(cmd)
        self.assertAllowed("pkill -f \"$PWD/\"", cwd=f"{wt}/sub")

    def test_kill_denial_says_what_to_run_instead(self):
        reason = self.assertDenied("pkill clang")
        self.assertIn("Run instead:", reason)
        self.assertIn("kill <PID>", reason)
        self.assertNotIn("jobserver", reason)  # not the build message

    def test_build_and_kill_in_one_command_report_both(self):
        reason = self.assertDenied(f"pkill clang; {INCIDENT}")
        self.assertIn("jobserver", reason)
        self.assertIn("selects processes by name or pattern", reason)

    def test_brief_with_by_name_kill_denied(self):
        for text in ("Clean up with `pkill -f bezel_tests` first.",
                     "2. Kill the codegen build you started: `pkill -f 'cmake.*--build.*lane-57-5201'`.",
                     "If a run hangs: pgrep cmake | xargs kill",
                     "Step 4: kill $(pgrep clang)",
                     "killall cc1plus if the build stalls"):
            with self.subTest(text=text):
                reason = self.agent(text)
                self.assertIsNotNone(reason)
                self.assertIn("selects processes by name or pattern", reason)

    def test_brief_that_forbids_or_scopes_is_allowed(self):
        for text in ("Name-scoped pkill is machine-wide; PID-scoped kill preferred.",
                     # lines from dc's own notice to the peer, refused by the first version
                     "REFUSED for every lane: patterns by name (`pkill -f bezel_tests_codegen`), "
                     "killall, `kill $(pgrep …)`, `ps|grep|xargs kill`.",
                     "ALLOWED: `kill <PID>`, `pgrep -f '/mnt/fast/worktrees/<wt>/x' | xargs kill`.",
                     "Never use pkill/killall by name; kill by PID.",
                     "Name-scoped kills (`pkill clang`) are FORBIDDEN.",
                     "Kill your own build: pkill -f \"$(readlink -f /mnt/fast/worktrees/x/build)/\"",
                     "Stop it with `pkill -P 1234`.",
                     "Wait with `kill -0 PID` in a loop.",
                     # narrative, not an instruction (replayed from real peer messages)
                     "My apologies for the kill of pid 3853030 — my `pgrep -f 'x.*y'` regex matched.",
                     "Explicit warning against pkill-by-name. Lane will report the kill.",
                     "Pattern `while pgrep -f X; do sleep N` hangs; kill the loop by PID.",
                     "The reviewer killed the stray with a name-scoped pkill."):
            with self.subTest(text=text):
                self.assertIsNone(self.agent(text))


class PlumbingTest(HookTestBase):
    def test_malformed_input_exits_zero_silently(self):
        for raw in ("", "not json", "[]", '{"tool_name": "Bash", "tool_input": null}'):
            with self.subTest(raw=raw):
                p = self.run_hook(None, raw=raw)
                self.assertEqual(p.stdout, "")

    def test_other_tools_ignored(self):
        self.assertIsNone(self.decision({"tool_name": "Read", "cwd": self.repo,
                                         "tool_input": {"file_path": INCIDENT}}))

    def test_denial_is_logged(self):
        self.assertDenied(INCIDENT)
        with open(self.log) as f:
            rows = [json.loads(line) for line in f]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["session"], "s1")
        self.assertEqual(rows[0]["input"], INCIDENT)

    def test_internal_error_fails_open(self):
        orig = bg.Guard.check_script
        try:
            bg.Guard.check_script = lambda *a, **k: 1 / 0
            self.assertIsNone(bg.decide_safely({"tool_name": "Bash", "cwd": self.repo,
                                                "tool_input": {"command": INCIDENT}},
                                               [self.repo]))
        finally:
            bg.Guard.check_script = orig

    def test_deep_nesting_terminates(self):
        # Linear in depth. (Re-quoting a payload per level doubles its
        # backslashes: 40 levels of that is 2**40 bytes, which OOM-killed the
        # test itself while this file was written.)
        self.bash("echo " + "$(echo " * 200 + "true" + ")" * 200)
        self.bash("bash -c " * 200 + "true")
        # A build below the depth limit is still found.
        self.assertDenied("echo " + "$(true; " * 3 + INCIDENT + ")" * 3)


if __name__ == "__main__":
    unittest.main()
