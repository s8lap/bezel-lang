// The machine-wide build jobserver, enforced for OpenCode sessions.
//
// WHY. The build token pool (scripts/build_jobserver.py) and the memory-capped
// slice (scripts/dev_heavy.sh) only bind a build that reaches them. A bare
// `cmake --build build -j16` in a lane worktree runs outside both: on
// 2026-09-30 that reached 499 concurrent clang and the kernel OOM killer took
// three sessions and plasmashell. scripts/claude_hooks/build_guard.py is the
// audited rule set that stops it, and the Claude Code hook runs it for Claude
// Code sessions. This plugin is the same rule set for OpenCode sessions.
//
// ONE RULE SET. The rules live in scripts/claude_hooks/build_guard.py, not
// here. This file asks that script what to do with a shell command (its
// `--route` mode) and does what it says. The script's pin suite is
// scripts/gate/tests/test_build_guard_hook.py.
//
// ROUTE, DO NOT REFUSE. The Claude Code hook refuses a bare build and names the
// command to run instead. A refusal costs the lane a round-trip, and an agent
// that does not understand the refusal may retry with a slightly different
// bare build. So this plugin routes instead:
//
//   * a bare `cmake --build ...`, or `make`/`ninja` in a CMake build dir, is
//     REWRITTEN to `scripts/dev_heavy.sh build <tool> ...`: the same build dir,
//     the same targets, the same output paths, but the build joins the
//     machine-wide token pool and runs inside the memory-capped slice.
//   * a build the rewriter cannot see (a payload that is not a single build
//     command, a build behind a wrapper prefix) is not rewritten, but the
//     jobserver env (MAKEFLAGS with the FIFO auth, BEZEL_JOBSERVER=on, no
//     CMAKE_BUILD_PARALLEL_LEVEL) is injected, so the build still takes a
//     token per compile.
//   * a pool opt-out (BEZEL_JOBSERVER=off, a hand-set MAKEFLAGS, a
//     --jobserver-auth) and a by-name kill are REFUSED. Neither can be
//     routed: a kill is not a build, and opting out of a pool shared by every
//     lane is not the lane's decision to make.
//
// SCOPE. This plugin covers shell COMMANDS. It does not lint lane briefs the
// way the Claude Code hook lints Write/Edit of a file named *brief*: a C++ test
// file whose name contains "brief" would be linted as prose, and a false
// positive there would block real lane work.

import { execFileSync } from "node:child_process"
import { existsSync, readFileSync, statSync } from "node:fs"
import path from "node:path"

const REPO = "/home/stepan/w/code/bezel-lang"

const GUARD_CANDIDATES = [
  process.env.BEZEL_BUILD_GUARD,
  path.join(REPO, "scripts/claude_hooks/build_guard.py"),
  path.join(process.env.HOME || "", ".claude/hooks/bezel_build_guard.py"),
].filter(Boolean)

const ROOTS = [REPO, "/var/tmp/bezel-scratch", "/mnt/fast"]

const JOBSERVER_DIR = process.env.BEZEL_JOBSERVER_DIR || "/var/tmp/bezel-scratch/jobserver"

// The shell tool. OpenCode v2 names it "shell"; "bash" is the v1 name, still
// accepted here so a renamed or aliased tool is covered too.
const SHELL_TOOLS = new Set(["shell", "bash"])

// A command that mentions a build tool: the only kind worth asking the guard
// about a second time (the env injection).
const MENTIONS_BUILD = /(^|[\s;&|()'"=<>])((cmake|make|gmake|ninja|ninja-build))([\s;&|()'"]|$)/

function guardPath() {
  for (const candidate of GUARD_CANDIDATES) {
    try {
      if (existsSync(candidate)) return candidate
    } catch {}
  }
  return null
}

function jobserverAlive() {
  try {
    const pid = Number(readFileSync(path.join(JOBSERVER_DIR, "pid"), "utf8").trim())
    if (!Number.isFinite(pid) || pid <= 0) return false
    process.kill(pid, 0)
    return statSync(path.join(JOBSERVER_DIR, "fifo")).isFIFO()
  } catch {
    return false
  }
}

// Ask the rule set what to do with one command. A broken rule set must never
// stop a session: the token launcher still bounds the build.
function decide(command, cwd) {
  const guard = guardPath()
  if (!guard) return { action: "allow" }
  try {
    const args = [guard, "--route"]
    for (const root of ROOTS) args.push("--root", root)
    const out = execFileSync("python3", args, {
      input: JSON.stringify({ command, cwd }),
      encoding: "utf8",
      timeout: 20000,
      maxBuffer: 4 * 1024 * 1024,
    })
    const parsed = JSON.parse(out)
    if (!parsed || typeof parsed.action !== "string") return { action: "allow" }
    return parsed
  } catch (e) {
    return { action: "allow" }
  }
}

function applyEnv(invocation, env) {
  if (!env || typeof env !== "object") return
  for (const [key, value] of Object.entries(env)) {
    if (typeof value === "string") invocation.env[key] = value
  }
  // cmake would pass its own -j to make, which drops the inherited jobserver.
  delete invocation.env.CMAKE_BUILD_PARALLEL_LEVEL
}

// The decision for the shell call the tool hook just handled, so the shell
// hook does not have to ask the rule set a second time.
let lastDecision = null

export default {
  id: "bezel-build-guard",
  async setup(ctx) {
    const cwdOf = (input) => {
      if (input && typeof input.workdir === "string" && input.workdir) return input.workdir
      if (ctx && ctx.location && typeof ctx.location.directory === "string") return ctx.location.directory
      return process.cwd()
    }

    await ctx.tool.hook("execute.before", async (event) => {
      if (!event || !SHELL_TOOLS.has(event.tool)) return
      const input = event.input
      if (!input || typeof input.command !== "string" || !input.command.trim()) return
      const decision = decide(input.command, cwdOf(input))
      lastDecision = { command: input.command, decision }
      if (decision.action === "deny") {
        throw new Error(decision.reason || "build_guard: refused.")
      }
      if (decision.action === "route" && typeof decision.command === "string" && decision.command !== input.command) {
        input.command = decision.command
        lastDecision = { command: decision.command, decision }
      }
    })

    await ctx.shell.hook("create.before", async (invocation) => {
      if (!invocation || typeof invocation.command !== "string") return
      if (!invocation.env || typeof invocation.env !== "object") return
      let decision = lastDecision && lastDecision.command === invocation.command ? lastDecision.decision : null
      if (!decision) {
        // A shell call that did not come through the shell tool.
        if (!MENTIONS_BUILD.test(invocation.command)) return
        decision = decide(invocation.command, invocation.cwd || process.cwd())
      }
      if (!decision || decision.action !== "route") return
      if (!decision.env) return
      if (!jobserverAlive()) return
      applyEnv(invocation, decision.env)
    })
  },
}
