"""Throwaway git repos for the coordinator-tooling tests.

NOT a test module (the name does not match `test*.py`), so it is imported,
never collected.

Every repo lives in a TemporaryDirectory and runs git with a pinned identity
and no global/system config, so a test can never read or write the real repo,
the real hooks dir, or the operator's git config. The scripts under test are
run from THIS checkout (SCRIPTS) against the temp repo, which is the shape the
coordinator uses them in (from one worktree, against another).
"""
import os
import pathlib
import shutil
import subprocess

SCRIPTS = pathlib.Path(__file__).resolve().parent.parent.parent
WANT = "Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"


def git_env(tmp):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("GIT_"):
            del env[k]
    env.update({
        "GIT_AUTHOR_NAME": "Test Author", "GIT_AUTHOR_EMAIL": "author@example.invalid",
        "GIT_COMMITTER_NAME": "Test Author", "GIT_COMMITTER_EMAIL": "author@example.invalid",
        "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
        "TMPDIR": str(tmp),
    })
    return env


class Repo:
    """A temp git repo on branch `main` with one initial commit."""

    def __init__(self, root, env):
        self.root = pathlib.Path(root)
        self.env = env
        self.root.mkdir(parents=True, exist_ok=True)
        self.git("init", "-q", "-b", "main")
        self.write("README", "init\n")
        self.commit("init", ["README"])

    def git(self, *args, check=True, env=None):
        return subprocess.run(["git", "-C", str(self.root)] + list(args),
                              env=env or self.env, check=check,
                              capture_output=True, text=True)

    def out(self, *args):
        return self.git(*args).stdout.strip()

    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def commit(self, msg, paths, check=True, env=None):
        for rel in paths:
            self.git("add", "--", rel)
        return self.git("commit", "-q", "-m", msg, check=check, env=env)

    def change(self, rel, msg, text=None):
        """Write `rel` and commit it with message `msg`; return the new sha."""
        self.write(rel, text if text is not None else msg + "\n")
        self.commit(msg, [rel])
        return self.out("rev-parse", "HEAD")

    def copy_scripts(self, *rels):
        """Commit copies of this checkout's scripts/<rel> into the repo."""
        for rel in rels:
            dst = self.root / "scripts" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(SCRIPTS / rel, dst)
            self.git("add", "--", "scripts/" + rel)
        self.git("commit", "-q", "-m", "add scripts\n\n" + WANT)


def run(script, *args, cwd, env):
    return subprocess.run([str(SCRIPTS / script)] + list(args), cwd=str(cwd),
                          env=env, capture_output=True, text=True)
