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
  key for a binary whose source tree has any uncommitted or untracked path.

Runs no build and no test binary: the "binary" is a stub script that answers
the identity probe, over a throwaway git repository.
"""

import os
import re
import subprocess
import tempfile
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

    def test_any_artefact_change_changes_the_key(self):
        _, first, _ = self._keys()
        self.keys.unlink()
        (self.d / "rt.a").write_text("runtime rebuilt\n")
        _, second, _ = self._keys()
        self.assertNotEqual(first[0].split("\t")[2], second[0].split("\t")[2])


if __name__ == "__main__":
    unittest.main()
