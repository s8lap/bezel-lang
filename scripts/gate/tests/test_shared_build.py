"""scripts/shared_build.sh + scripts/agent_check.sh --use-shared (r39 infra).

shared_build.sh publishes bezelc and the test binaries for a commit, read-only,
under $BEZEL_SHARED_BUILD_ROOT/<sha>/, so a read-only lane (reviewer, issue
verifier) runs them instead of building its own. These tests pin the parts a
wrong answer would silently cost correctness on:

  * path/ensure never serve an incomplete build, and two concurrent ensures of
    one sha build it ONCE (the second waits on the flock);
  * resolve / agent_check --use-shared REFUSE a worktree whose src/ or tests/
    differ from the ref (tracked edits or untracked files) — otherwise a lane
    would run stale binaries against edited sources and report a false green;
  * --use-shared really runs no build, and --probe uses the shared bezelc;
  * the tree-key match (a commit that changed only docs/ reuses the build);
  * prune keeps the newest N and never removes a build a reader holds.

No real cmake/clang: every repo, build and binary is FAKE and lives under the
worktree's own scratch/ (never /tmp). The fake builder is the script's
TEST-ONLY hook BEZEL_SHARED_BUILD_TEST_BUILDER; the fake test binaries answer
BEZEL_TEST_ENUMERATE and print the harness's summary line, and embed their
build's bezelc path the way the real ones do (build_paths.cpp), which
shared_build.sh's verify step checks. agent_check.sh runs for real in the fake
repo, with a stub dev_heavy.sh that logs every call (a `build` line in that log
is a build that --use-shared promised not to run).

Stdlib only; runs in the gate-selftest phase.
"""

import fcntl
import os
import pathlib
import shutil
import stat
import subprocess
import textwrap
import unittest
import uuid

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent
WORKTREE = SCRIPTS.parent
SHARED_BUILD_SH = SCRIPTS / "shared_build.sh"
AGENT_CHECK_SH = SCRIPTS / "agent_check.sh"
SCRATCH = WORKTREE / "scratch" / "test_shared_build"

FAKE_BUILDER = r'''
set -e
: "${SB_BUILD:?}"
mkdir -p "$SB_BUILD/tests"
[ -z "${FAKE_BUILD_COUNT:-}" ] || echo "$SB_SHA" >> "$FAKE_BUILD_COUNT"
[ -z "${FAKE_BUILD_SLEEP:-}" ] || sleep "$FAKE_BUILD_SLEEP"
[ -z "${FAKE_BUILD_FAIL:-}" ] || exit 1
cat > "$SB_BUILD/bezelc" <<EOF
#!/usr/bin/env bash
echo "FAKE-BEZELC \$0 \$*" >> "\${FAKE_BEZELC_LOG:-/dev/null}"
out=""; while [ \$# -gt 0 ]; do [ "\$1" = -o ] && out="\$2"; shift; done
[ -z "\$out" ] || : > "\$out"
EOF
cat > "$SB_BUILD/tests/bezel_tests_fake" <<EOF
#!/usr/bin/env bash
# baked path, as build_paths.cpp embeds it: $SB_BUILD/bezelc
name=fake_suite.case_one
if [ -n "\${BEZEL_TEST_ENUMERATE:-}" ]; then
  case "\$name" in *"\${BEZEL_TEST_FILTER:-}"*) echo "\$name" ;; esac; exit 0
fi
echo "[       OK ] \$name (from $SB_BUILD)"
echo "1 passed, 0 failed, 1 total"
EOF
chmod +x "$SB_BUILD/bezelc" "$SB_BUILD/tests/bezel_tests_fake"
printf 'fake\t%s\t1\n' "$SB_BUILD/tests/bezel_tests_fake" > "$SB_BUILD/tests/test_binaries.txt"
'''

STUB_DEV_HEAVY = r'''#!/usr/bin/env bash
kind="$1"; shift
echo "$kind $*" >> "${FAKE_DH_LOG:?}"
if [ "$kind" = build ]; then echo "stub dev_heavy: refusing a real build" >&2; exit 1; fi
exec "$@"
'''

GIT_ENV = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
           "GIT_CONFIG_NOSYSTEM": "1"}


def _force_rmtree(p):
    def onerr(func, path, _exc):
        os.chmod(os.path.dirname(path), stat.S_IRWXU)
        try:
            os.chmod(path, stat.S_IRWXU)
        except OSError:
            pass
        func(path)
    if p.exists():
        for dp, dns, fns in os.walk(p):
            for n in dns + fns:
                try:
                    os.chmod(os.path.join(dp, n), stat.S_IRWXU, follow_symlinks=False)
                except (OSError, NotImplementedError):
                    pass
        shutil.rmtree(p, onerror=onerr)


class SharedBuildTestBase(unittest.TestCase):

    def setUp(self):
        self.base = SCRATCH / uuid.uuid4().hex[:10]
        self.repo = self.base / "repo"
        self.root = self.base / "shared"
        self.logs = self.base / "logs"
        for d in (self.repo, self.logs):
            d.mkdir(parents=True)
        self.addCleanup(_force_rmtree, self.base)
        self.env = dict(os.environ)
        for k in list(self.env):
            if k.startswith(("BEZEL_", "GIT_")) or k in ("JOBS",):
                del self.env[k]
        self.env.update(GIT_ENV)
        self.env.update({
            "BEZEL_SHARED_BUILD_ROOT": str(self.root),
            "BEZEL_SHARED_BUILD_TEST_BUILDER": FAKE_BUILDER,
            "BEZEL_SHARED_BUILD_MIN_TOKENS": "0",
            "BEZEL_AGENT_SCRATCH": str(self.base / "agent"),
            "FAKE_DH_LOG": str(self.logs / "dev_heavy.log"),
            "FAKE_BEZELC_LOG": str(self.logs / "bezelc.log"),
            "FAKE_BUILD_COUNT": str(self.logs / "builds.log"),
            "AGENT_CHECK_JOBS": "1",
        })
        self.git("init", "-q", "-b", "main")
        (self.repo / "src").mkdir()
        (self.repo / "tests").mkdir()
        (self.repo / "docs").mkdir()
        (self.repo / "src" / "a.cpp").write_text("int a;\n")
        (self.repo / "tests" / "t.cpp").write_text("int t;\n")
        (self.repo / "docs" / "d.md").write_text("doc\n")
        (self.repo / "CMakeLists.txt").write_text("# fake\n")
        (self.repo / ".gitignore").write_text("build/\nscratch/\n")
        self.git("add", "src", "tests", "docs", "CMakeLists.txt", ".gitignore")
        self.git("commit", "-q", "-m", "c1")
        # The scripts under test, plus a stub dev_heavy.sh. Left untracked
        # (scripts/ is not part of the tree key).
        sd = self.repo / "scripts"
        sd.mkdir()
        for src in (SHARED_BUILD_SH, AGENT_CHECK_SH):
            shutil.copy2(src, sd / src.name)
        (sd / "dev_heavy.sh").write_text(STUB_DEV_HEAVY)
        (sd / "dev_heavy.sh").chmod(0o755)

    def git(self, *args):
        r = subprocess.run(["git", "-C", str(self.repo), *args], env=self.env,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, "git %s failed: %s" % (args, r.stderr))
        return r.stdout.strip()

    def commit_file(self, rel, text, msg):
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        self.git("add", rel)
        self.git("commit", "-q", "-m", msg)
        return self.git("rev-parse", "HEAD")

    def sb(self, *args, env=None, timeout=60):
        return subprocess.run(["bash", str(self.repo / "scripts" / "shared_build.sh"), *args],
                              cwd=self.repo, env=env or self.env, capture_output=True,
                              text=True, timeout=timeout)

    def ac(self, *args, timeout=60):
        return subprocess.run(["bash", str(self.repo / "scripts" / "agent_check.sh"), *args],
                              cwd=self.repo, env=self.env, capture_output=True,
                              text=True, timeout=timeout)

    def builds(self):
        p = self.logs / "builds.log"
        return p.read_text().split() if p.exists() else []

    def dh_kinds(self):
        p = self.logs / "dev_heavy.log"
        return [ln.split(" ", 1)[0] for ln in p.read_text().splitlines()] if p.exists() else []

    def publish_head(self):
        r = self.sb("ensure", "HEAD")
        self.assertEqual(r.returncode, 0, r.stderr)
        return pathlib.Path(r.stdout.strip().splitlines()[-1])


class SharedBuildPathEnsureTest(SharedBuildTestBase):

    def test_path_exits_1_when_nothing_is_published(self):
        r = self.sb("path", "HEAD")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(r.stdout, "")
        self.assertIn("no published build", r.stderr)

    def test_ensure_publishes_a_complete_read_only_build_with_manifest(self):
        sha = self.git("rev-parse", "HEAD")
        d = self.publish_head()
        self.assertEqual(d, self.root / sha)
        r = self.sb("path", "HEAD")
        self.assertEqual((r.returncode, r.stdout.strip()), (0, str(d)))
        m = dict(ln.split("=", 1) for ln in (d / "MANIFEST").read_text().splitlines())
        self.assertEqual(m["sha"], sha)
        self.assertEqual(m["treekey"], self.sb("treekey", "HEAD").stdout.strip())
        self.assertEqual(m["build_dir"], str(d / "tree" / "build"))
        self.assertIn("md5.bezelc", m)
        self.assertIn("md5.tests/bezel_tests_fake", m)
        self.assertEqual(m["tree.src"], self.git("rev-parse", "HEAD:src"))
        self.assertEqual(m["tree.cmake"], "-")
        self.assertTrue((d / "test_binaries.txt").is_file())
        self.assertTrue(os.access(d / "bezelc", os.X_OK))
        bezelc = d / "tree" / "build" / "bezelc"
        self.assertFalse(os.stat(bezelc).st_mode & 0o222, "published bezelc must be read-only")
        # a second ensure is a no-op
        self.assertEqual(self.sb("ensure", "HEAD").returncode, 0)
        self.assertEqual(self.builds(), [sha])

    def test_concurrent_ensures_of_one_sha_build_it_once(self):
        env = dict(self.env, FAKE_BUILD_SLEEP="2")
        script = self.repo / "scripts" / "shared_build.sh"
        procs = [subprocess.Popen(["bash", str(script), "ensure", "HEAD"], cwd=self.repo,
                                  env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True) for _ in range(2)]
        outs = [p.communicate(timeout=60) for p in procs]
        for p, (o, e) in zip(procs, outs):
            self.assertEqual(p.returncode, 0, e)
        dirs = {o.strip().splitlines()[-1] for o, _ in outs}
        self.assertEqual(len(dirs), 1, outs)
        self.assertEqual(len(self.builds()), 1,
                         "two ensures of one sha must build it ONCE: %r" % self.builds())

    def test_failed_build_publishes_nothing_and_a_later_ensure_rebuilds(self):
        r = self.sb("ensure", "HEAD", env=dict(self.env, FAKE_BUILD_FAIL="1"))
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(self.sb("path", "HEAD").returncode, 1,
                         "an incomplete build must never be served")
        self.publish_head()
        self.assertEqual(self.sb("path", "HEAD").returncode, 0)
        self.assertEqual(len(self.builds()), 2)

    def test_tree_key_match_reuses_a_build_across_a_docs_only_commit(self):
        old = self.git("rev-parse", "HEAD")
        self.publish_head()
        new = self.commit_file("docs/d.md", "changed\n", "docs only")
        self.assertEqual(self.sb("path", new).returncode, 1)
        r = self.sb("path", "--tree", new)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, str(self.root / old)), r.stderr)
        r = self.sb("resolve", "auto")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("match=tree", r.stdout)
        self.assertIn("sha=%s" % old, r.stdout)
        # a src/ change is a different key
        self.commit_file("src/a.cpp", "int a2;\n", "src")
        self.assertEqual(self.sb("path", "--tree", "HEAD").returncode, 1)

    def test_prune_keeps_newest_n_and_skips_a_build_in_use(self):
        shas = [self.git("rev-parse", "HEAD")]
        self.publish_head()
        for i in range(3):
            shas.append(self.commit_file("src/a.cpp", "int a%d;\n" % i, "c%d" % i))
            self.publish_head()
        # hold a reader lock on the oldest, then prune to 2
        # (held in THIS process, exactly as agent_check.sh holds it: flock -s)
        lock = self.root / ".locks" / ("%s.use" % shas[0])
        fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o644)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_SH)
        r = self.sb("prune", "--keep", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.root / shas[0] / "MANIFEST").exists(), "in-use build was pruned")
        self.assertFalse((self.root / shas[1]).exists(), "an old unused build survived prune")
        self.assertTrue((self.root / shas[2] / "MANIFEST").exists())
        self.assertTrue((self.root / shas[3] / "MANIFEST").exists())
        self.assertNotIn(str(self.root / shas[1]), self.git("worktree", "list"))


class SharedBuildHardeningTest(SharedBuildTestBase):
    """Review notes of r39-rev-sharedbuild (probes P-foreign, P10, check_clean)."""

    def test_prune_and_publish_leave_non_sha_dirs_alone(self):
        self.publish_head()
        foreign = self.root / "not-a-build"
        foreign.mkdir()
        (foreign / "keep.txt").write_text("precious\n")
        short = self.root / "abc123"      # hex but not 40 chars
        short.mkdir()
        upper = self.root / ("A" * 40)    # 40 chars but not lowercase hex
        upper.mkdir()
        r = self.sb("prune", "--keep", "0")
        self.assertEqual(r.returncode, 0, r.stderr)
        for d in (foreign, short, upper):
            self.assertTrue(d.exists(), "prune removed non-build dir %s" % d)
        self.assertTrue((foreign / "keep.txt").exists())
        # publishing another commit must not touch them either
        self.commit_file("src/a.cpp", "int z;\n", "z")
        self.publish_head()
        self.assertTrue((foreign / "keep.txt").exists())

    def test_prune_still_reaps_an_abandoned_partial_sha_dir(self):
        sha = self.git("rev-parse", "HEAD")
        r = self.sb("ensure", "HEAD", env=dict(self.env, FAKE_BUILD_FAIL="1"))
        self.assertNotEqual(r.returncode, 0)
        self.assertTrue((self.root / sha).exists())
        self.assertEqual(self.sb("prune").returncode, 0)
        self.assertFalse((self.root / sha).exists())

    def test_root_slash_empty_or_missing_is_refused(self):
        for val in ("/", ""):
            r = self.sb("prune", env=dict(self.env, BEZEL_SHARED_BUILD_ROOT=val))
            self.assertEqual(r.returncode, 2, (val, r.stderr))
        r = self.sb("prune", env=dict(self.env, BEZEL_SHARED_BUILD_ROOT=str(self.base / "nope")))
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertFalse((self.base / "nope").exists())

    def test_prune_never_unlinks_lock_files(self):
        shas = [self.git("rev-parse", "HEAD")]
        self.publish_head()
        shas.append(self.commit_file("src/a.cpp", "int q;\n", "q"))
        self.publish_head()
        locks = [self.root / ".locks" / ("%s.%s" % (shas[0], k)) for k in ("build", "use")]
        inodes = {l: os.stat(l).st_ino for l in locks if l.exists()}
        self.assertTrue(inodes)
        r = self.sb("prune", "--keep", "1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((self.root / shas[0]).exists())
        for l, ino in inodes.items():
            self.assertTrue(l.exists(), "prune unlinked lock %s" % l)
            self.assertEqual(os.stat(l).st_ino, ino)

    def test_check_clean_refuses_when_git_diff_fails(self):
        self.publish_head()
        # corrupt the index so `git diff` exits non-zero
        (self.repo / ".git" / "index").write_bytes(b"garbage")
        r = self.sb("check-clean", "HEAD")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("REFUSED", r.stderr)

    def test_toolchain_mismatch_warns_but_serves(self):
        d = self.publish_head()
        m = d / "MANIFEST"
        txt = m.read_text().replace("toolchain=", "toolchain=OLD-CLANG ", 1)
        m.chmod(0o644)
        m.write_text(txt)
        r = self.sb("path", "HEAD")
        self.assertEqual((r.returncode, r.stdout.strip()), (0, str(d)), r.stderr)
        if shutil.which("clang++"):
            self.assertIn("toolchain mismatch", r.stderr)


class SharedBuildRefusalTest(SharedBuildTestBase):

    def test_resolve_refuses_a_tracked_src_edit(self):
        self.publish_head()
        (self.repo / "src" / "a.cpp").write_text("int edited;\n")
        r = self.sb("resolve", "HEAD")
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertIn("REFUSED", r.stderr)
        self.assertIn("src/a.cpp", r.stderr)
        self.assertNotIn("dir=", r.stdout)

    def test_resolve_refuses_an_untracked_tests_file(self):
        self.publish_head()
        (self.repo / "tests" / "new_test.cpp").write_text("int n;\n")
        r = self.sb("resolve", "auto")
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertIn("tests/new_test.cpp", r.stderr)

    def test_resolve_refuses_a_ref_whose_tree_differs_from_the_worktree(self):
        old = self.git("rev-parse", "HEAD")
        self.publish_head()
        self.commit_file("tests/t.cpp", "int t2;\n", "tests changed")
        r = self.sb("resolve", old)
        self.assertEqual(r.returncode, 3, "a clean tree at a NEWER commit must not run "
                         "an older ref's binaries: %s" % r.stderr)


class AgentCheckUseSharedTest(SharedBuildTestBase):

    def test_use_shared_runs_published_binaries_with_no_build(self):
        d = self.publish_head()
        r = self.ac("--area", "fake", "case_one", "--use-shared", "HEAD")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("ALL requested filters green", r.stdout)
        self.assertIn("from %s" % (d / "tree" / "build"), r.stdout)
        self.assertNotIn("build", self.dh_kinds(), "--use-shared ran a build")
        self.assertIn("suite", self.dh_kinds())

    def test_use_shared_refuses_a_dirty_tree_and_runs_nothing(self):
        self.publish_head()
        (self.repo / "src" / "a.cpp").write_text("int edited;\n")
        r = self.ac("--use-shared", "HEAD", "case_one")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("REFUSED", r.stderr)
        self.assertIn("Nothing was run", r.stderr)
        self.assertEqual(self.dh_kinds(), [])

    def test_use_shared_auto_with_nothing_published_fails_with_the_ensure_hint(self):
        r = self.ac("--use-shared", "auto", "case_one")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("shared_build.sh ensure", r.stderr)
        self.assertEqual(self.dh_kinds(), [])

    def test_probe_with_use_shared_uses_the_shared_bezelc(self):
        d = self.publish_head()
        bzl = self.base / "p.bzl"
        bzl.write_text("func main() {}\n")
        r = self.ac("--probe", str(bzl), "--use-shared", "HEAD")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        log = (self.logs / "bezelc.log").read_text()
        self.assertIn(str(d / "tree" / "build" / "bezelc"), log)
        self.assertNotIn("build", self.dh_kinds())

    def test_lit_is_refused_with_use_shared(self):
        self.publish_head()
        r = self.ac("--use-shared", "HEAD", "--lit", "case_one")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--lit cannot run with --use-shared", r.stderr)

    def test_default_mode_offers_a_matching_build_but_still_builds(self):
        d = self.publish_head()
        r = self.ac("case_one")
        self.assertIn("NOTE: a published build matches this clean tree: %s" % d, r.stderr)
        self.assertIn("build", self.dh_kinds(),
                      "the offer must never replace the build without --use-shared")
        # a dirty tree gets no offer
        (self.logs / "dev_heavy.log").unlink()
        (self.repo / "src" / "a.cpp").write_text("int edited;\n")
        r = self.ac("case_one")
        self.assertNotIn("NOTE: a published build", r.stderr)


if __name__ == "__main__":
    unittest.main()
