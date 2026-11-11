"""The harness RESULTS cache (tests/support/results_cache.hpp, keyed by
scripts/results_cache.sh, driven by scripts/agent_check.sh) must NEVER serve
the gate: the gate is the acceptance oracle, and a served PASS there would
accept a tree whose tests were never run on it.

The guarantee has three layers, and this module pins the two that live in
shell (the third, "the harness honours BEZEL_RESULTS_CACHE=0 even with a valid
key file, and does nothing at all without one", is pinned in C++ by
tests/support/results_cache_test.cpp
`served_pass_skips_body_and_off_switch_executes`):

  1. every gate entry point and tests/run_parallel.sh exports
     BEZEL_RESULTS_CACHE=0 and unsets BEZEL_RESULTS_CACHE_KEYS at TOP LEVEL,
     right after its `set -u`, before it can run anything — so a caller's
     exported key file or mode cannot leak into an acceptance run, and
     pregate.sh's `--filters` (which runs agent_check.sh) is uncached too;
  2. under BEZEL_RESULTS_CACHE=0, scripts/agent_check.sh's rc_init turns the
     cache off even when `--results-cache` was passed, and
     scripts/results_cache.sh refuses to emit a key;
  plus the clean-tree rule that keeps a fail-without arm (a reverted fix in
  the working tree) and work in progress uncached: results_cache.sh emits no
  key for a binary whose source tree has any uncommitted or untracked path,
  re-keys every run (an earlier key line is never reused), and `commit` drops
  a run's staged passes when, AFTER the run, the tree is dirty, an artefact
  changed, or a tracked path changed during it (review r42-rev-infra-rcache F2).

Runs no build and no test binary: the "binary" is a stub script that answers
the identity probe, over a throwaway git repository.
"""

import os
import re
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RC_SH = ROOT / "scripts" / "results_cache.sh"
AGENT_CHECK = ROOT / "scripts" / "agent_check.sh"

GATE_ENTRY_POINTS = [
    "scripts/gate.sh",
    "scripts/gate_full_cycle.sh",
    "scripts/gate_and_push.sh",
    "scripts/pregate.sh",
    "tests/run_parallel.sh",
]


def _scratch_parent() -> str:
    d = os.environ.get("TMPDIR") or str(ROOT / "scratch")
    os.makedirs(d, exist_ok=True)
    return d


class GateScriptsForceTheCacheOff(unittest.TestCase):
    def test_every_entry_point_exports_off_at_top_level_before_running_anything(self):
        for rel in GATE_ENTRY_POINTS:
            with self.subTest(script=rel):
                lines = (ROOT / rel).read_text().splitlines()
                set_u = next(i for i, l in enumerate(lines) if re.match(r"^set -u", l))
                exp = [i for i, l in enumerate(lines) if l == "export BEZEL_RESULTS_CACHE=0"]
                uns = [i for i, l in enumerate(lines) if l == "unset BEZEL_RESULTS_CACHE_KEYS"]
                self.assertTrue(exp, f"{rel}: no top-level `export BEZEL_RESULTS_CACHE=0`")
                self.assertTrue(uns, f"{rel}: no top-level `unset BEZEL_RESULTS_CACHE_KEYS`")
                # Right after `set -u`: only comments in between, so nothing
                # (no function call, no exec of a test binary) precedes it.
                for i in range(set_u + 1, exp[0]):
                    self.assertTrue(lines[i].startswith("#") or not lines[i].strip(),
                                    f"{rel}:{i + 1}: code before the results-cache export: {lines[i]!r}")
                self.assertEqual(uns[0], exp[0] + 1, rel)
                # Nothing later in the script turns it back on.
                for i, l in enumerate(lines):
                    if re.search(r"BEZEL_RESULTS_CACHE(_KEYS)?=", l) and i != exp[0] and not l.lstrip().startswith("#"):
                        self.fail(f"{rel}:{i + 1}: re-assigns the results cache: {l!r}")

    def test_entry_point_environment_is_off_when_the_caller_exported_a_key(self):
        # Execute each script's own prologue (from line 1 through the unset)
        # under a caller environment that tries to switch the cache on.
        for rel in GATE_ENTRY_POINTS:
            with self.subTest(script=rel):
                lines = (ROOT / rel).read_text().splitlines()
                end = lines.index("unset BEZEL_RESULTS_CACHE_KEYS")
                prologue = "\n".join(l for l in lines[: end + 1] if not l.startswith("#!"))
                prologue = "\n".join(l for l in prologue.splitlines()
                                     if re.match(r"^(set |export BEZEL_RESULTS|unset BEZEL_RESULTS)", l))
                env = dict(os.environ, BEZEL_RESULTS_CACHE="1", BEZEL_RESULTS_CACHE_KEYS="/nonexistent/keys")
                out = subprocess.run(
                    ["bash", "-c", prologue + '\necho "mode=[${BEZEL_RESULTS_CACHE-}] keys=[${BEZEL_RESULTS_CACHE_KEYS-unset}]"'],
                    env=env, capture_output=True, text=True, check=True).stdout
                self.assertIn("mode=[0] keys=[unset]", out, rel)


class AgentCheckHonoursTheOffSwitch(unittest.TestCase):
    def _rc_block(self) -> str:
        s = AGENT_CHECK.read_text()
        a = s.index("# --- the harness RESULTS cache")
        b = s.index("# --- probe mode")
        return s[a:b]

    def _state(self, flag: str, mode):
        with tempfile.TemporaryDirectory(dir=_scratch_parent()) as d:
            env = {k: v for k, v in os.environ.items() if not k.startswith("BEZEL_RESULTS_CACHE")}
            env["BEZEL_RESULTS_CACHE_DIR"] = os.path.join(d, "cache")
            if mode is not None:
                env["BEZEL_RESULTS_CACHE"] = mode
            script = (f"ROOT={ROOT}\nSCRATCH={d}\nRC_FLAG={flag}\n" + self._rc_block() +
                      '\nrc_init\necho "state=[$RC_STATE] why=[$RC_WHY]"\n')
            return subprocess.run(["bash", "-c", script], env=env, capture_output=True,
                                  text=True, check=True).stdout

    def test_off_switch_wins_over_the_flags(self):
        for flag in ("on", "verify", '""'):
            with self.subTest(flag=flag):
                self.assertIn("state=[off] why=[BEZEL_RESULTS_CACHE=0]", self._state(flag, "0"))

    def test_flags_select_the_mode(self):
        self.assertIn("state=[rw]", self._state("on", None))
        self.assertIn("state=[verify]", self._state("verify", None))
        self.assertIn("state=[off] why=[unrecognised", self._state('""', "bogus"))

    def test_default_is_on_for_a_lane_run(self):
        # The rollout: no flag and no BEZEL_RESULTS_CACHE serves + records.
        self.assertIn("state=[rw]", self._state('""', None))


class ResultsCacheKeyRules(unittest.TestCase):
    SCHEMA = "bezel.results-cache.v1"

    def setUp(self):
        self._td = tempfile.TemporaryDirectory(dir=_scratch_parent())
        self.d = Path(self._td.name)
        self.repo = self.d / "repo"
        self.repo.mkdir()
        genv = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                    GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
        (self.repo / "f.txt").write_text("x\n")
        for cmd in (["git", "init", "-q"], ["git", "add", "f.txt"], ["git", "commit", "-q", "-m", "init"]):
            subprocess.run(cmd, cwd=self.repo, env=genv, check=True, capture_output=True)
        # A stub "test binary" that answers the identity probe like test_main.cpp.
        self.bin = self.d / "bezel_tests_stub"
        self.bin.write_text(
            "#!/usr/bin/env bash\n"
            '[ -n "${BEZEL_RESULTS_CACHE_DESCRIBE-}" ] || exit 0\n'
            f'echo "schema={self.SCHEMA}"\n'
            "echo \"exe_stat=$(stat -L -c '%d:%i:%s:%.9Y' \"$0\")\"\n"
            f'echo "repo_root={self.repo}"\n'
            f'echo "bezelc={self.d}/bezelc"\necho "runtime_lib={self.d}/rt.a"\necho "core_lib={self.d}/core.a"\n')
        self.bin.chmod(0o755)
        for f in ("bezelc", "rt.a", "core.a"):
            (self.d / f).write_text(f)
        self.keys = self.d / "keys.tsv"

    def tearDown(self):
        self._td.cleanup()

    def _keys(self, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith("BEZEL_RESULTS_CACHE")}
        env["BEZEL_RESULTS_CACHE_DIR"] = str(self.d / "cache")
        env["BEZEL_RESULTS_CACHE_MIN_FREE_GB"] = "0"
        env.update(extra)
        r = subprocess.run([str(RC_SH), "keys", str(self.keys), str(self.bin)], env=env,
                           capture_output=True, text=True)
        lines = self.keys.read_text().splitlines() if self.keys.exists() else []
        return r.returncode, lines, r.stderr

    def test_clean_tree_gets_a_key_bound_to_the_binary_stat(self):
        rc, lines, err = self._keys()
        self.assertEqual(rc, 0, err)
        self.assertEqual(len(lines), 1, err)
        schema, stat, key, path = lines[0].split("\t")
        self.assertEqual(schema, self.SCHEMA)
        self.assertEqual(stat, subprocess.run(["stat", "-L", "-c", "%d:%i:%s:%.9Y", str(self.bin)],
                                              capture_output=True, text=True).stdout.strip())
        self.assertRegex(key, r"^[0-9a-f]{64}$")

    def test_off_switch_emits_no_key(self):
        for v in ("0", "off"):
            with self.subTest(v=v):
                rc, lines, err = self._keys(BEZEL_RESULTS_CACHE=v)
                self.assertEqual(rc, 1)
                self.assertEqual(lines, [])
                self.assertIn(f"BEZEL_RESULTS_CACHE={v}", err)

    def test_dirty_or_untracked_tree_emits_no_key(self):
        (self.repo / "f.txt").write_text("reverted fix\n")
        rc, lines, err = self._keys()
        self.assertEqual((rc, lines), (1, []), err)
        self.assertIn("uncommitted/untracked", err)
        subprocess.run(["git", "checkout", "-q", "--", "f.txt"], cwd=self.repo, check=True)
        (self.repo / "new_probe.bzl").write_text("x\n")
        rc, lines, err = self._keys()
        self.assertEqual((rc, lines), (1, []), err)

    def test_byte_identical_relink_keeps_the_key_with_a_fresh_stat(self):
        _, first, _ = self._keys()
        self.keys.unlink()
        st = self.bin.stat()
        os.utime(self.bin, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
        _, second, _ = self._keys()
        self.assertNotEqual(first[0].split("\t")[1], second[0].split("\t")[1])  # stat id moved
        self.assertEqual(first[0].split("\t")[2], second[0].split("\t")[2])     # content key did not

    def test_any_artefact_change_changes_the_key(self):
        _, first, _ = self._keys()
        self.keys.unlink()
        (self.d / "rt.a").write_text("runtime rebuilt\n")
        _, second, _ = self._keys()
        self.assertNotEqual(first[0].split("\t")[2], second[0].split("\t")[2])
        self.keys.unlink()
        with open(self.bin, "a") as f:  # the test binary's own bytes change
            f.write("# relinked with a change\n")
        _, third, _ = self._keys()
        self.assertNotIn(third[0].split("\t")[2], {first[0].split("\t")[2], second[0].split("\t")[2]})


    def _rc(self, *args, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith("BEZEL_RESULTS_CACHE")}
        env["BEZEL_RESULTS_CACHE_DIR"] = str(self.d / "cache")
        env["BEZEL_RESULTS_CACHE_MIN_FREE_GB"] = "0"
        env.update(extra)
        return subprocess.run([str(RC_SH), *map(str, args)], env=env, capture_output=True, text=True)

    def _staged_run(self, during=None):
        """stage -> keys -> (the 'run': one staged pass, then `during`) -> commit."""
        (self.d / "cache" / "v1" / ("a" * 64) / "e.pass").unlink(missing_ok=True)
        stage = Path(self._rc("stage").stdout.strip())
        self.assertTrue((stage / ".started").exists())
        self._rc("keys", self.keys, self.bin)
        hk = stage / ("a" * 64)
        hk.mkdir()
        (hk / "e.pass").write_text("staged pass\n")
        if during:
            time.sleep(0.05)  # file times are jiffy-grained
            during()
        r = self._rc("commit", stage, self.keys, self.bin)
        self.assertFalse(stage.exists(), "the stage is removed either way")
        promoted = (self.d / "cache" / "v1" / ("a" * 64) / "e.pass").exists()
        return r, promoted

    def test_commit_promotes_the_stage_of_a_run_on_an_unchanged_tree(self):
        r, promoted = self._staged_run()
        self.assertEqual((r.returncode, r.stdout.strip()), (0, "promoted 1"), r.stderr)
        self.assertTrue(promoted)

    def test_commit_drops_the_stage_when_the_tree_went_dirty_during_the_run(self):
        cases = {
            "untracked": lambda: (self.repo / "new_probe.bzl").write_text("x\n"),
            "modified": lambda: (self.repo / "f.txt").write_text("reverted fix\n"),
            # Edited and restored before the run ended: git status is clean
            # again, but the path changed during the run.
            "edit-and-revert": lambda: (self.repo / "f.txt").write_text("x\n"),
            "artefact-rebuilt": lambda: (self.d / "rt.a").write_text("runtime rebuilt\n"),
        }
        for name, during in cases.items():
            with self.subTest(case=name):
                r, promoted = self._staged_run(during)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertTrue(r.stdout.startswith("dropped 1 ("), r.stdout)
                self.assertIn("DROPPED 1 new pass(es)", r.stderr)
                self.assertFalse(promoted, name)
                # Restore for the next case.
                subprocess.run(["git", "checkout", "-q", "--", "f.txt"], cwd=self.repo, check=True)
                (self.repo / "new_probe.bzl").unlink(missing_ok=True)
                (self.d / "rt.a").write_text("rt.a")
        # Control on the restored tree: promoted again.
        r, promoted = self._staged_run()
        self.assertEqual(r.stdout.strip(), "promoted 1", r.stderr)
        self.assertTrue(promoted)

    def test_a_tree_dirtied_after_the_first_key_loses_its_key_line(self):
        rc, lines, err = self._keys()
        self.assertEqual((rc, len(lines)), (0, 1), err)
        (self.repo / "new_probe.bzl").write_text("x\n")
        rc, lines, err = self._keys()  # the same OUT, as agent_check's next filter
        self.assertEqual((rc, lines), (1, []), err)

    def test_agent_check_commits_or_drops_each_runs_stage(self):
        block = AgentCheckHonoursTheOffSwitch._rc_block(None)
        for dirty, want in ((False, "promoted"), (True, "dropped")):
            with self.subTest(dirty=dirty):
                env = {k: v for k, v in os.environ.items() if not k.startswith("BEZEL_RESULTS_CACHE")}
                env["BEZEL_RESULTS_CACHE_DIR"] = str(self.d / "cache")
                env["BEZEL_RESULTS_CACHE_MIN_FREE_GB"] = "0"
                (self.d / "cache" / "v1" / ("b" * 64) / "e.pass").unlink(missing_ok=True)
                touch = f'sleep 0.05; : > "{self.repo}/new_probe.bzl"\n' if dirty else ""
                script = (f"ROOT={ROOT}\nSCRATCH={self.d}\nRC_FLAG=on\n" + block +
                          f'\nrc_init\nrc_env_for "{self.bin}"\n'
                          'printf "%s\\n" "${RC_ENV[@]}"\n'
                          'mkdir -p "$RC_STAGE/' + "b" * 64 + '" && echo p > "$RC_STAGE/' + "b" * 64 + '/e.pass"\n'
                          + touch +
                          f'rc_commit "{self.bin}"\nrc_report\n')
                out = subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True).stdout
                self.assertIn("BEZEL_RESULTS_CACHE_STAGE=" + str(self.d / "cache" / "stage" / "run."), out)
                landed = (self.d / "cache" / "v1" / ("b" * 64) / "e.pass").exists()
                self.assertEqual(landed, not dirty, out)
                if dirty:
                    self.assertIn("1 new pass(es) DROPPED", out)
                else:
                    self.assertNotIn("DROPPED", out)
                (self.repo / "new_probe.bzl").unlink(missing_ok=True)

if __name__ == "__main__":
    unittest.main()
