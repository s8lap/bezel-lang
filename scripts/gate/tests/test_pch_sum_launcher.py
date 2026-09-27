"""tests/pch_sum_launcher.sh: the test-PCH launcher for relocatable ccache (#5073).

The launcher compiles the PCH directly (never through ccache) and writes a
content-derived, tree-independent <pch>.sum that the consumers' ccache hashes
instead of the PCH bytes (ccache option pch_external_checksum=true). These tests pin the
properties that make that sound, using a fake compiler so they need no clang:

  - the .sum is equal across trees at different paths with identical inputs,
    although the PCH bytes differ (they embed the tree path, like clang's);
  - it changes when any input's CONTENT changes, or the argv changes;
  - a stale .sum never survives a PCH compile: it is removed before the
    compiler runs, so a failed compile or an unparsable depfile leaves none;
  - the PCH compile runs with CCACHE_DISABLE=1 and bypasses the ccache
    launcher; every other compile is handed to the launcher unchanged.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPO / "tests" / "pch_sum_launcher.sh"

FAKE_CLANG = r"""#!/usr/bin/env bash
if [ "$1" = --version ]; then echo "fakeclang ${FAKE_VERSION:-1.0}"; exit 0; fi
[ -n "${FAKE_FAIL:-}" ] && exit 3
prev=; out=; mf=; src=
for a; do case $prev in -o) out=$a;; -MF) mf=$a;; -c) src=$a;; esac; prev=$a; done
# Like clang, the "PCH" embeds the absolute input path.
echo "pch-of $src ccache_disable=${CCACHE_DISABLE:-}" > "$out"
printf '%s: %s \\\n  %s\n' "$out" "$src" "$FAKE_DEPS" > "$mf"
"""

FAKE_CCACHE = r"""#!/usr/bin/env bash
echo "ccache-called $*" >> "$FAKE_CCACHE_LOG"
exit 0
"""


class PchSumLauncherTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="pch-sum-launcher-")
        self.tmp = pathlib.Path(self._tmp.name)
        self.clang = self._exe("fakeclang", FAKE_CLANG)
        self.ccache = self._exe("fakeccache", FAKE_CCACHE)
        self.ccache_log = self.tmp / "ccache.log"
        # An out-of-tree "system" header shared by every tree.
        self.sys_hdr = self.tmp / "sys" / "vector"
        self.sys_hdr.parent.mkdir()
        self.sys_hdr.write_text("// system header v1\n")

    def tearDown(self):
        self._tmp.cleanup()

    def _exe(self, name, text):
        p = self.tmp / name
        p.write_text(text)
        p.chmod(0o755)
        return p

    def _tree(self, rel, hdr_text="#include <vector>\n"):
        tree = self.tmp / rel
        pchdir = tree / "build" / "tests" / "CMakeFiles" / "bezel_tests_pch.dir"
        pchdir.mkdir(parents=True)
        (pchdir / "cmake_pch.hxx").write_text(hdr_text)
        (pchdir / "cmake_pch.hxx.cxx").write_text("/* generated */\n")
        return tree

    def _run(self, tree, *, extra_args=(), env_extra=None, emit_pch=True):
        cwd = tree / "build" / "tests"
        d = "CMakeFiles/bezel_tests_pch.dir"
        hxx = cwd / d / "cmake_pch.hxx"
        cxx = cwd / d / "cmake_pch.hxx.cxx"
        out = f"{d}/cmake_pch.hxx.pch" if emit_pch else f"{d}/pch_owner.cpp.o"
        argv = [str(LAUNCHER), str(tree), str(tree), str(self.ccache),
                f"base_dir={tree}", "--", str(self.clang),
                f"-I{tree}/src", *extra_args]
        if emit_pch:
            argv += ["-Xclang", "-emit-pch", "-Xclang", "-include", "-Xclang",
                     str(hxx), "-x", "c++-header"]
        argv += ["-MD", "-MT", f"tests/{out}", "-MF", f"{out}.d",
                 "-o", out, "-c", str(cxx)]
        env = dict(os.environ, FAKE_DEPS=f"{hxx} {self.sys_hdr}",
                   FAKE_CCACHE_LOG=str(self.ccache_log))
        env.pop("CCACHE_DISABLE", None)
        env.update(env_extra or {})
        r = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True)
        return r, cwd / out

    def _sum(self, pch):
        s = pathlib.Path(str(pch) + ".sum")
        return s.read_text().strip() if s.exists() else None

    def test_sum_is_tree_independent_but_pch_bytes_are_not(self):
        r1, p1 = self._run(self._tree("a"))
        r2, p2 = self._run(self._tree("deeper/path/b"))
        self.assertEqual((r1.returncode, r2.returncode), (0, 0), r1.stderr + r2.stderr)
        self.assertNotEqual(p1.read_text(), p2.read_text())
        s1, s2 = self._sum(p1), self._sum(p2)
        self.assertRegex(s1 or "", r"^[0-9a-f]{64}$")
        self.assertEqual(s1, s2)

    def test_pch_bypasses_ccache(self):
        r, pch = self._run(self._tree("a"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ccache_disable=1", pch.read_text())
        self.assertFalse(self.ccache_log.exists())

    def test_non_pch_compile_goes_through_the_ccache_launcher(self):
        tree = self._tree("a")
        r, obj = self._run(tree, emit_pch=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        log = self.ccache_log.read_text()
        self.assertIn(f"base_dir={tree} {self.clang} ", log)
        self.assertFalse(obj.exists())  # the fake ccache compiled nothing

    def test_input_content_change_changes_the_sum(self):
        _, p1 = self._run(self._tree("a"))
        _, p2 = self._run(self._tree("b", hdr_text="#include <vector>\n#define X 1\n"))
        self.assertNotEqual(self._sum(p1), self._sum(p2))
        # An out-of-tree (system) header edit too, in the SAME tree.
        tree = self._tree("c")
        _, p3 = self._run(tree)
        before = self._sum(p3)
        self.sys_hdr.write_text("// system header v2\n")
        _, p3 = self._run(tree)
        self.assertIsNotNone(self._sum(p3))
        self.assertNotEqual(before, self._sum(p3))

    def test_argv_and_compiler_version_change_the_sum(self):
        _, p1 = self._run(self._tree("a"))
        _, p2 = self._run(self._tree("b"), extra_args=["-DFOO=1"])
        _, p3 = self._run(self._tree("c"), env_extra={"FAKE_VERSION": "2.0"})
        sums = {self._sum(p1), self._sum(p2), self._sum(p3)}
        self.assertEqual(len(sums), 3)
        self.assertNotIn(None, sums)

    def test_failed_compile_removes_stale_sum_and_propagates_rc(self):
        tree = self._tree("a")
        _, pch = self._run(tree)
        self.assertIsNotNone(self._sum(pch))
        r, pch = self._run(tree, env_extra={"FAKE_FAIL": "1"})
        self.assertEqual(r.returncode, 3)
        self.assertIsNone(self._sum(pch))

    def test_unusable_depfile_removes_stale_sum_and_fails_closed(self):
        tree = self._tree("a")
        _, pch = self._run(tree)
        self.assertIsNotNone(self._sum(pch))
        # A depfile entry that does not exist: the compile succeeds, but no
        # .sum may describe inputs the launcher could not hash.
        r, pch = self._run(tree, env_extra={"FAKE_DEPS": str(self.tmp / "gone.h")})
        self.assertEqual(r.returncode, 0)
        self.assertIn("no ", r.stderr)
        self.assertIsNone(self._sum(pch))
        # An escaped path is refused rather than mis-split.
        _, pch = self._run(tree)
        self.assertIsNotNone(self._sum(pch))
        r, pch = self._run(tree, env_extra={"FAKE_DEPS": "a\\ b.h"})
        self.assertEqual(r.returncode, 0)
        self.assertIsNone(self._sum(pch))


if __name__ == "__main__":
    unittest.main()
