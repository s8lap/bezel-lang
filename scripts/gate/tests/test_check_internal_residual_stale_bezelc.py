"""Tests for scripts/check_internal_residual.py #5011 fail-closed guards.

Pins two related fixes:

  1. find_bezelc() MUST NOT silently fall back to sibling lane-build
     dirs when ROOT/build/bezelc is missing. Pre-fix the script
     scanned /var/tmp/bezel-scratch/lane-builds/* and
     /mnt/fast/lane-builds/* for *any* executable `bezelc`, silently
     picking up another lane's stale binary - the vacuous-oracle class
     of defect this script exists to detect. The fix exits 2 with a
     clear "no usable bezelc found" message instead.

  2. check_staleness() MUST fail closed when build/bezelc is older
     than the newest source file in src/. Pre-fix the script never
     checked staleness; a lane that edited src/ but did not rebuild
     bezelc silently measured the OLD compiler against the NEW tree.

Subprocess strategy: check_internal_residual.py's ROOT is computed at
import time as `pathlib.Path(__file__).resolve().parent.parent`. To
make the script walk a synthetic src/ tree, the subprocess tests COPY
the script into the fixture and run it from there - so the script's
ROOT is the fixture. In-process tests patch the module's ROOT.

MTIME DETERMINISM: every test sets file mtimes against a single
BASELINE (seconds-since-epoch, computed once at fixture init) so the
ordering is a function of the offset passed to write_*, NOT of
wall-clock time between sequential touches. Without this, a slow test
machine could see delta > the offset between two fixtures' mtimes and
silently flip an assertion.
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent.parent
SRC_SCRIPT = SCRIPTS_DIR / 'check_internal_residual.py'


class _Fixture:
    """Build a minimal repo/ tree the script can walk.

    Layout (mirrors what the real script expects):
      <root>/src/<sub>/<file>.{cpp,hpp,h,...}
      <root>/build/bezelc
      <root>/scripts/check_internal_residual.py  (copy of SRC_SCRIPT)

    Mtimes are set against a single BASELINE (computed once at fixture
    init, far enough in the past that no offset underflows). The
    offset is in seconds since BASELINE; positive = newer. This keeps
    the ordering deterministic regardless of how long the fixture
    takes to build.
    """

    _OFFSET_SECONDS = 10

    def __init__(self):
        self.root = tempfile.mkdtemp(prefix='cir-stale-test-')
        self._t0 = time.time() - (10 ** 6)
        # Copy the script into the fixture so its ROOT (computed as
        # `__file__.parent.parent`) resolves to the fixture, and the
        # production src/ tree is never probed.
        scripts_dst = pathlib.Path(self.root) / 'scripts'
        scripts_dst.mkdir(parents=True, exist_ok=True)
        self.script_path = scripts_dst / 'check_internal_residual.py'
        shutil.copy2(str(SRC_SCRIPT), str(self.script_path))

    def repo(self):
        return self.root

    def _set_mtime(self, path, offset_units):
        mtime = self._t0 + offset_units * self._OFFSET_SECONDS
        # Both atime and mtime; the predicate uses mtime.
        os.utime(path, (mtime, mtime))

    def write_src(self, rel, mtime_offset=20):
        """Plant a src/ source file at the given mtime offset.

        mtime_offset=20 means "20 units newer than the fixture's
        t0 baseline"; the default models the post-lane-commit edit.
        """
        path = os.path.join(self.root, 'src', rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write('// test fixture\n')
        self._set_mtime(path, mtime_offset)
        return path

    def write_bezelc(self, mtime_offset=0, body=None):
        """Plant an executable build/bezelc at the given mtime offset.

        mtime_offset=0 means "at the fixture's t0 baseline"; the
        default models a freshly-built bezelc. A test that needs a
        STALE bezelc passes mtime_offset < the source's offset.
        """
        path = os.path.join(self.root, 'build', 'bezelc')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(body if body is not None else '#!/bin/sh\necho fake\n')
        os.chmod(path, 0o755)
        self._set_mtime(path, mtime_offset)
        return path

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


def _run_script(args, fixture, env_extra=None, timeout=30):
    """Run the fixture's copy of check_internal_residual.py.

    `args` is a list of CLI args. `env_extra` is merged into the
    inherited environment. The script is invoked with `sys.executable`
    so the test sees the same Python as the developer's `python3` on
    the path. We use `python3` (not the script's shebang line) to
    avoid surprises if the shebang line is wrong.
    """
    full_env = dict(os.environ)
    if env_extra:
        full_env.update(env_extra)
    proc = subprocess.run(
        [sys.executable, str(fixture.script_path)] + list(args),
        capture_output=True, text=True,
        env=full_env, timeout=timeout,
    )
    return proc.returncode, proc.stdout, proc.stderr


class NoLaneBuildFallbackTest(unittest.TestCase):
    """find_bezelc() refuses to scan sibling lane-build dirs (#5011).

    Pre-fix the script scanned /var/tmp/bezel-scratch/lane-builds/*
    and /mnt/fast/lane-builds/* when ROOT/build/bezelc was missing,
    silently picking up another lane's stale bezelc. The post-fix
    script exits 2 with a clear "no usable bezelc found" message
    instead.

    The fixture is set up with NO build/bezelc at all (no write_bezelc
    call). find_bezelc() then has only the explicitly-passed candidate
    to consider; if THAT doesn't exist either, it exits 2.
    """

    def setUp(self):
        self.fx = _Fixture()

    def tearDown(self):
        self.fx.cleanup()

    def test_missing_root_bezelc_fails_closed(self):
        """ROOT/build/bezelc absent + --bezelc to /no/such: exit 2."""
        # The fixture has no build/ subdir by construction. We pass
        # --bezelc and BEZELC to non-existent paths so find_bezelc()
        # rejects all candidates and exits 2 with the canonical "no
        # usable bezelc found" message. The script is invoked with
        # --strict so it takes the normal measure+check path, NOT
        # --self-test which bypasses find_bezelc entirely.
        rc, _out, err = _run_script(
            ['--bezelc', '/no/such/bezelc', '--strict'],
            fixture=self.fx,
            env_extra={'BEZELC': '/no/such/bezelc'},
        )
        self.assertEqual(rc, 2,
            'missing bezelc must fail closed with exit 2; '
            'stderr=%r' % err)
        self.assertIn('no usable bezelc', err,
            'error message must say "no usable bezelc"; got '
            'stderr=%r' % err)

    def test_explicit_flag_passes_through(self):
        """--bezelc PATH takes priority and reaches --self-test."""
        bezelc = self.fx.write_bezelc(mtime_offset=20)
        rc, _out, _err = _run_script(
            ['--bezelc', str(bezelc), '--self-test'],
            fixture=self.fx,
        )
        self.assertEqual(rc, 0,
            '--bezelc=%s + --self-test must succeed; '
            'stderr=%r' % (bezelc, _err))


class StaleBezelcRefusedTest(unittest.TestCase):
    """check_staleness() refuses a build/bezelc older than newest src/.

    The pre-fix script never checked staleness; a lane that edited
    src/ but did not rebuild bezelc silently measured the OLD
    compiler against the NEW tree - the silent-wrong-code class of
    defect this detector exists to catch.
    """

    def setUp(self):
        self.fx = _Fixture()

    def tearDown(self):
        self.fx.cleanup()

    def test_stale_bezelc_exits_2_with_stale_message(self):
        """bezelc older than src/foo.cpp: exit 2 + STALE message."""
        self.fx.write_src('foo/bar.cpp', mtime_offset=20)
        bezelc = self.fx.write_bezelc(mtime_offset=0)
        rc, _out, err = _run_script(['--bezelc', str(bezelc)],
                                    fixture=self.fx)
        self.assertEqual(rc, 2,
            'stale bezelc must exit 2; stderr=%r' % err)
        self.assertIn('STALE', err,
            'error must say STALE so an operator recognizes it; '
            'stderr=%r' % err)
        self.assertIn('src/foo/bar.cpp', err,
            'error must name the offending source file; '
            'stderr=%r' % err)

    def test_stale_bezelc_does_not_compile(self):
        """check_staleness() runs BEFORE measure(); a stale bezelc
        fails before any corpus file is compiled.

        Plant a fake bezelc that exits 99 on any call (a sentinel
        that would be observable as exit code 99 IF measure()
        executed it). Then assert the script exits 2 - NOT 99 -
        which means measure() never forked the fake.
        """
        self.fx.write_src('foo/bar.cpp', mtime_offset=20)
        bezelc_path = self.fx.write_bezelc(mtime_offset=0)
        # Replace the body with a sentinel that exits 99 IF invoked.
        # The 'w' open bumps mtime to NOW; reset it to mtime_offset=0
        # so the staleness check still fires against src/ at
        # mtime_offset=20.
        with open(bezelc_path, 'w') as f:
            f.write('#!/bin/sh\nexit 99\n')
        os.chmod(bezelc_path, 0o755)
        self.fx._set_mtime(bezelc_path, 0)
        rc, _out, err = _run_script(['--bezelc', str(bezelc_path)],
                                    fixture=self.fx)
        self.assertEqual(rc, 2,
            'stale bezelc must exit 2 (NOT execute the fake); '
            'rc=%d stderr=%r' % (rc, err))
        self.assertNotEqual(rc, 99,
            'check_staleness must run BEFORE measure() so the '
            'fake bezelc never executes; got rc=99 (measure ran)')
        self.assertIn('STALE', err,
            'must have STALE message; stderr=%r' % err)


class FreshBezelcPassesTest(unittest.TestCase):
    """check_staleness() does NOT reject a fresh bezelc.

    The negative of the #5011 fix: a bezelc newer than every src/
    file must not trigger the staleness guard. The script then
    proceeds to measure(); with a fake bezelc + empty corpus, the
    script reports a clean result (no STALE HEADROOM, no DROP -
    there's nothing to measure).

    The structural invariant: exit code is NOT 2 (no fail-closed
    guard fired), stderr does NOT contain the STALE message.
    """

    def setUp(self):
        self.fx = _Fixture()

    def tearDown(self):
        self.fx.cleanup()

    def test_fresh_bezelc_does_not_trigger_staleness(self):
        """bezelc newer than every src/ file: no STALE message."""
        # Plant an EMPTY src/ dir (no source files at all) so the
        # staleness predicate has nothing to compare against - by
        # definition bezelc cannot be older than nothing.
        os.makedirs(os.path.join(self.fx.repo(), 'src'), exist_ok=True)
        bezelc = self.fx.write_bezelc(mtime_offset=20)
        # Bootstrap a baseline file in the fixture so check() can
        # read it (otherwise read_baseline fails closed with its own
        # exit 2, which would mask the assertion we care about).
        baseline_path = os.path.join(self.fx.repo(), 'scripts',
                                     'internal_residual.baseline')
        with open(baseline_path, 'w') as f:
            f.write('total_internal_lines = 0\n')
        rc, _out, err = _run_script(['--bezelc', str(bezelc)],
                                    fixture=self.fx)
        self.assertNotEqual(rc, 2,
            'fresh bezelc must NOT exit 2 (no fail-closed guard '
            'fired); rc=%d stderr=%r' % (rc, err))
        self.assertNotIn('STALE', err,
            'fresh bezelc must NOT produce STALE message; '
            'stderr=%r' % err)


class InProcessHelpersTest(unittest.TestCase):
    """Direct unit tests for find_bezelc() and check_staleness().

    Subprocess tests above verify the CLI integration. These tests
    verify the helper functions in isolation, with a patched ROOT
    pointing at the fixture, so a regression in either helper is
    pinned regardless of CLI plumbing.
    """

    def setUp(self):
        self.fx = _Fixture()
        # Import the script as a module and patch its module-level
        # ROOT to point at the fixture so find_bezelc() / check_staleness()
        # probe the fixture, not the production tree.
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            '_cir_test_module', str(SRC_SCRIPT))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.cir = mod
        self._saved_root = mod.ROOT
        mod.ROOT = pathlib.Path(self.fx.repo())

    def tearDown(self):
        self.cir.ROOT = self._saved_root
        self.fx.cleanup()

    def test_find_bezelc_returns_explicit(self):
        """--bezelc PATH takes priority."""
        bezelc = self.fx.write_bezelc(mtime_offset=20)
        self.assertEqual(
            os.path.realpath(self.cir.find_bezelc(
                explicit=str(bezelc), root=pathlib.Path(self.fx.repo()))),
            os.path.realpath(bezelc),
        )

    def test_find_bezelc_returns_root_bezelc(self):
        """ROOT/build/bezelc is the fallback."""
        bezelc = self.fx.write_bezelc(mtime_offset=20)
        found = self.cir.find_bezelc(
            explicit=None, root=pathlib.Path(self.fx.repo()))
        self.assertEqual(os.path.realpath(found), os.path.realpath(bezelc))

    def test_find_bezelc_fails_closed_when_missing(self):
        """Missing ROOT/build/bezelc + no explicit + no $BEZELC: exit 2."""
        # No write_bezelc call -> build/ is empty.
        old_env = os.environ.pop('BEZELC', None)
        try:
            with self.assertRaises(SystemExit) as cm:
                self.cir.find_bezelc(
                    explicit=None, root=pathlib.Path(self.fx.repo()))
            self.assertEqual(cm.exception.code, 2,
                'missing bezelc must exit 2; got %r' % cm.exception.code)
        finally:
            if old_env is not None:
                os.environ['BEZELC'] = old_env

    def test_check_staleness_silent_on_fresh(self):
        """Fresh bezelc: silent (no exception)."""
        self.fx.write_src('foo/bar.cpp', mtime_offset=0)
        bezelc = pathlib.Path(self.fx.write_bezelc(mtime_offset=20))
        # No exception expected.
        self.cir.check_staleness(
            bezelc, root=pathlib.Path(self.fx.repo()))

    def test_check_staleness_fails_closed_on_stale(self):
        """Stale bezelc: exit 2 with a STALE message."""
        self.fx.write_src('foo/bar.cpp', mtime_offset=20)
        bezelc = pathlib.Path(self.fx.write_bezelc(mtime_offset=0))
        with self.assertRaises(SystemExit) as cm:
            self.cir.check_staleness(
                bezelc, root=pathlib.Path(self.fx.repo()))
        self.assertEqual(cm.exception.code, 2)

    def test_check_staleness_message_names_source(self):
        """The error message names the offending source file."""
        import io as _io
        from contextlib import redirect_stderr as _rs
        self.fx.write_src('foo/bar.cpp', mtime_offset=20)
        bezelc = pathlib.Path(self.fx.write_bezelc(mtime_offset=0))
        buf = _io.StringIO()
        try:
            with _rs(buf):
                self.cir.check_staleness(
                    bezelc, root=pathlib.Path(self.fx.repo()))
        except SystemExit:
            pass
        msg = buf.getvalue()
        self.assertIn('STALE', msg,
            'error must say STALE; got %r' % msg)
        self.assertIn('src/foo/bar.cpp', msg,
            'error must name the offending source file; got %r' % msg)


if __name__ == '__main__':
    unittest.main()
