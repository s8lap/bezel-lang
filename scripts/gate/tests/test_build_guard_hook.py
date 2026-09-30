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
        path = os.path.join(self.repo, "docs", "coordinator-runbook.md")
        self.assertIsNone(self.decision({"tool_name": "Write", "cwd": self.repo,
                                         "tool_input": {"file_path": path,
                                                        "content": f"{INCIDENT}\n"}}))


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
