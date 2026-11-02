"""Tests for the route mode of scripts/claude_hooks/build_guard.py.

The Claude Code hook REFUSES a build that would skip the machine-wide
jobserver and names the command to run instead. The OpenCode plugin
(scripts/claude_hooks/opencode_build_guard.js) asks the same rule set what to
do with a shell command and ROUTES it instead: the bare build is rewritten
into the `scripts/dev_heavy.sh build ...` form, which joins the token pool and
runs inside the memory-capped slice, with the same build dir, the same targets
and the same output paths.

Pin groups:
  1. Routed: a bare `cmake --build ...` and `make`/`ninja` in a CMake build
     dir become `dev_heavy.sh build <tool> ...`; the build dir, the targets,
     the quoted arguments and the redirections survive; a `cd build && make`
     keeps its `cd`; a `bash -c "<build>"` and a `dev_heavy.sh build <wrapper>`
     become the pooled shape.
  2. Not rewritten but pooled: a build the rewriter cannot see (a wrapper
     prefix, a `$( )` payload) is left alone and still gets the jobserver env.
  3. Refused: a pool opt-out and a by-name kill cannot be routed.
  4. Allowed: the sanctioned paths, configure, dry runs, out-of-root commands.
Every case runs the real script as a subprocess, the way the plugin runs it.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = os.path.abspath(os.path.join(HERE, "..", "..", "claude_hooks", "build_guard.py"))


class RouteTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="build-guard-route-test-")
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

    def tearDown(self):
        self._tmp.cleanup()

    def route(self, command, cwd=None):
        p = subprocess.run(
            [sys.executable, HOOK, "--route", "--root", self.repo, "--root", self.plain],
            input=json.dumps({"command": command, "cwd": cwd or self.repo}),
            capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def assertRouted(self, command, cwd=None):
        d = self.route(command, cwd)
        self.assertEqual(d["action"], "route", f"not routed: {command!r} -> {d}")
        return d

    def assertAllowed(self, command, cwd=None):
        d = self.route(command, cwd)
        self.assertEqual(d["action"], "allow", f"not allowed: {command!r} -> {d}")
        return d

    def assertDenied(self, command, cwd=None, kind=None):
        d = self.route(command, cwd)
        self.assertEqual(d["action"], "deny", f"not denied: {command!r} -> {d}")
        if kind:
            self.assertEqual(d.get("kind"), kind, d)
        self.assertIn("reason", d)
        return d


class RoutedBuildsTest(RouteTestBase):
    def test_bare_cmake_build_becomes_dev_heavy(self):
        d = self.assertRouted("cmake --build build --target bezel_tests -j16")
        self.assertEqual(d["command"],
                         f"{self.repo}/scripts/dev_heavy.sh build cmake --build build "
                         "--target bezel_tests -j16")

    def test_make_in_build_dir_becomes_dev_heavy(self):
        d = self.assertRouted("make -C build -j8")
        self.assertEqual(d["command"],
                         f"{self.repo}/scripts/dev_heavy.sh build make -C build -j8")

    def test_ninja_in_build_dir_becomes_dev_heavy(self):
        d = self.assertRouted("ninja -C build")
        self.assertIn(f"{self.repo}/scripts/dev_heavy.sh build ninja -C build", d["command"])

    def test_build_dir_and_targets_survive(self):
        d = self.assertRouted("cmake --build build --target bezel_tests_sema")
        self.assertIn("--build build", d["command"])
        self.assertIn("--target bezel_tests_sema", d["command"])

    def test_quoted_argument_keeps_its_quotes(self):
        d = self.assertRouted('cmake --build "my build" -j4')
        self.assertIn('--build "my build"', d["command"])

    def test_redirection_survives_outside_the_rewrite(self):
        d = self.assertRouted("cmake --build build > out.log 2>&1")
        self.assertTrue(d["command"].endswith("> out.log 2>&1"), d["command"])

    def test_cd_before_the_build_is_kept(self):
        d = self.assertRouted("cd build && make -j8")
        self.assertTrue(d["command"].startswith("cd build &&"), d["command"])
        self.assertIn("dev_heavy.sh build make -j8", d["command"])

    def test_shell_payload_build_becomes_dev_heavy(self):
        d = self.assertRouted('bash -c "cmake --build build"')
        self.assertEqual(d["command"],
                         f"{self.repo}/scripts/dev_heavy.sh build cmake --build build")

    def test_dev_heavy_build_with_wrapper_payload_becomes_dev_heavy(self):
        d = self.assertRouted('scripts/dev_heavy.sh build bash -c "cmake --build build"')
        self.assertEqual(d["command"],
                         f"{self.repo}/scripts/dev_heavy.sh build cmake --build build")

    def test_dev_heavy_wrong_kind_build_becomes_a_build(self):
        d = self.assertRouted("scripts/dev_heavy.sh probe cmake --build build")
        self.assertEqual(d["command"],
                         f"{self.repo}/scripts/dev_heavy.sh build cmake --build build")

    def test_absolute_build_dir_resolves_to_its_own_tree(self):
        other = os.path.join(self.root, "other")
        os.makedirs(os.path.join(other, "scripts"))
        os.makedirs(os.path.join(other, "build"))
        with open(os.path.join(other, "scripts", "dev_heavy.sh"), "w") as f:
            f.write("#!/bin/sh\n")
        with open(os.path.join(other, "build", "CMakeCache.txt"), "w") as f:
            f.write(f"CMAKE_HOME_DIRECTORY:INTERNAL={other}\n")
        d = self.assertRouted(f"cmake --build {other}/build --target bezel_tests")
        self.assertTrue(d["command"].startswith(f"{other}/scripts/dev_heavy.sh build "),
                        d["command"])


class PooledButNotRewrittenTest(RouteTestBase):
    def test_wrapper_prefix_is_left_alone_but_gets_the_env(self):
        d = self.assertRouted("nohup cmake --build build -j8 &")
        self.assertEqual(d["command"], "nohup cmake --build build -j8 &")
        self.assertIn("note", d)
        self.assertIn("MAKEFLAGS", d["env"])
        self.assertIn("BEZEL_JOBSERVER", d["env"])

    def test_nested_payload_is_left_alone_but_gets_the_env(self):
        d = self.assertRouted("echo $(cmake --build build)")
        self.assertEqual(d["command"], "echo $(cmake --build build)")
        self.assertIn("MAKEFLAGS", d["env"])

    def test_relative_build_dir_without_a_tree_is_left_alone(self):
        # No dev_heavy.sh to point at: a prefix that does not exist would break
        # the build, so the command keeps its own words and gets the env.
        d = self.assertRouted("cmake --build build --target bezel_tests",
                              cwd=self.plain)
        self.assertEqual(d["command"], "cmake --build build --target bezel_tests")
        self.assertIn("MAKEFLAGS", d["env"])

    def test_env_names_the_pool_and_opts_in(self):
        d = self.assertRouted("cmake --build build")
        self.assertEqual(d["env"]["BEZEL_JOBSERVER"], "on")
        self.assertIn("--jobserver-auth=fifo:", d["env"]["MAKEFLAGS"])


class RefusedTest(RouteTestBase):
    def test_pool_opt_out_is_refused(self):
        d = self.assertDenied("BEZEL_JOBSERVER=off cmake --build build", kind="opt-out")
        self.assertIn("BEZEL_JOBSERVER", d["reason"])

    def test_hand_set_makeflags_is_refused(self):
        self.assertDenied("MAKEFLAGS=-j64 make -C build", kind="opt-out")

    def test_jobserver_auth_is_refused(self):
        self.assertDenied("make --jobserver-auth=fifo:/tmp/x -C build", kind="opt-out")

    def test_by_name_kill_is_refused(self):
        d = self.assertDenied("pkill -f bezelc", kind="kill")
        self.assertIn("kill", d["reason"])


class AllowedTest(RouteTestBase):
    def test_sanctioned_paths_are_allowed(self):
        self.assertAllowed("scripts/dev_heavy.sh build cmake --build build")
        self.assertAllowed("scripts/agent_check.sh --area sema Foo")

    def test_configure_and_dry_runs_are_allowed(self):
        self.assertAllowed("cmake -S . -B build")
        self.assertAllowed("make -n -C build")

    def test_plain_command_is_allowed(self):
        self.assertAllowed("ls -la")

    def test_out_of_root_command_is_allowed(self):
        self.assertAllowed("cmake --build build -j4", cwd=self.outside)


class PlumbingTest(RouteTestBase):
    def test_malformed_input_exits_zero_silently(self):
        p = subprocess.run([sys.executable, HOOK, "--route"], input="not json",
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(p.stdout.strip(), "")

    def test_a_bare_command_string_is_accepted(self):
        # A bare JSON string is the command; the cwd is the process's own.
        p = subprocess.run(
            [sys.executable, HOOK, "--route", "--root", self.repo],
            input=json.dumps("cmake --build build"), capture_output=True, text=True,
            timeout=30, cwd=self.repo)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout)["action"], "route")


if __name__ == "__main__":
    unittest.main()
