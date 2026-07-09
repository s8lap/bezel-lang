#pragma once

// Test support for the codegen milestone. Runs the full front-end on a source
// string, lowers it to an LLVM module, and JIT-executes `main` via LLVM ORC,
// returning the integer it produced (the program's exit code). JIT keeps the
// tests hermetic and fast — no external assembler/linker step.

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

// §22.13 / #1212: flush any static owned objects registered by this JIT run
// while the LLJIT is still alive.  Defined in bezel_runtime.cpp; linked into
// the test binary via DynamicLibrarySearchGenerator (the JIT resolves it from
// the host process).  Without calling this, the process-exit atexit handler
// runs after the LLJIT is destroyed and vtable pointers are dangling.
extern "C" void bz_flush_static_owned(void);

namespace bezel::test {

struct JitResult {
    DiagnosticEngine diagnostics;
    bool ran = false;     // true once `main` executed
    int exitCode = 0;     // value returned by `main`
};

// Initialize the native target exactly once per process (required before the
// ORC JIT can emit machine code for the host).
inline void ensureNativeTargetInitialized() {
    static const bool once = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        return true;
    }();
    (void)once;
}

// Lex + parse + sema, then lower and JIT-run, IN THE CALLING PROCESS. On any
// front-end error (or JIT failure) `ran` stays false; callers assert on
// `diagnostics`/`ran`/`exitCode`. `opts` allows per-call codegen options
// (default = unchecked release mode).
//
// This is the raw in-process pipeline. runJitWithOptions (below) forks and
// invokes this in a child so the parent never accumulates the child's LLVM /
// heap state; the child hands the JitResult back over an anonymous temp file.
inline JitResult runJitInProcess(std::string_view source,
                                 const codegen::CodegenOptions& opts) {
    ensureNativeTargetInitialized();
    JitResult r;

    Lexer lexer(source, r.diagnostics);
    Parser parser(lexer, r.diagnostics);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return r;

    sema::SemaContext ctx(r.diagnostics);
    sema::collectSymbols(*unit, ctx);
    sema::resolveTypes(*unit, ctx);
    sema::typecheckExpressions(*unit, ctx);
    if (r.diagnostics.hasErrors()) return r;

    // §22.3 migration gate (env-var BEZEL_CHECK_OWNERSHIP=1, off by default):
    // run the ownership pass and, on failure, print the source + diagnostics so
    // the test corpus can be audited for pre-v0.169 bare owned-transfer sites
    // (issue #1401). The marker is surface-only — emitted objects are
    // byte-identical — so this check does NOT block JIT execution; it only
    // reports so that the caller can see which programs need migration.
    {
        static const bool checkOwn =
            (std::getenv("BEZEL_CHECK_OWNERSHIP") != nullptr &&
             std::string(std::getenv("BEZEL_CHECK_OWNERSHIP")) == "1");
        if (checkOwn) {
            DiagnosticEngine ownDiags;
            sema::SemaContext ownCtx(ownDiags);
            // Re-parse for an independent ctx so we don't disturb `r`.
            Lexer ownLex(source, ownDiags);
            Parser ownPar(ownLex, ownDiags);
            auto ownUnit = ownPar.parseCompilationUnit();
            if (ownUnit) {
                sema::collectSymbols(*ownUnit, ownCtx);
                sema::resolveTypes(*ownUnit, ownCtx);
                sema::typecheckExpressions(*ownUnit, ownCtx);
                ownership::checkOwnership(*ownUnit, ownCtx);
            }
            if (ownDiags.hasErrors()) {
                // Print the source so a human/script can map line:col back.
                std::string src(source);
                std::fprintf(stderr,
                             "\n===OWNERSHIP_FAIL_BEGIN===\n%s\n"
                             "===OWNERSHIP_FAIL_DIAGS===\n",
                             src.c_str());
                for (const auto& d : ownDiags.diagnostics()) {
                    if (d.severity != Severity::Error) continue;
                    std::fprintf(stderr, "%u:%u: %s\n",
                                 d.location.line, d.location.column,
                                 d.message.c_str());
                }
                std::fprintf(stderr, "===OWNERSHIP_FAIL_END===\n");
            }
        }
    }

    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = codegen::emitModule(*unit, ctx, *llvmCtx, opts);
    if (!module) return r;

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        llvm::consumeError(jitOrErr.takeError());
        return r;
    }
    auto jit = std::move(*jitOrErr);

    // Resolve external symbols (calloc, and FFI calls like putchar/abs)
    // against the host process so extern declarations can be invoked.
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }

    // ThreadSafeModule takes ownership of both the module and its context.
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return r;
    }

    auto sym = jit->lookup("main");
    if (!sym) {
        llvm::consumeError(sym.takeError());
        return r;
    }
    auto fn = sym->toPtr<int (*)()>();
    r.exitCode = fn();
    // §22.13 / #1212: dispose any static owned fields registered during this
    // JIT run while the LLJIT is still alive (vtable pointers valid).  The
    // atexit handler installed by bz_register_static_owned would otherwise
    // fire after the LLJIT is destroyed → dangling vtable → SIGSEGV.
    bz_flush_static_owned();
    r.ran = true;
    return r;
}

namespace jit_isolate {
// ---------------------------------------------------------------------------
// Process isolation for runJit. Over ~11k serial tests the single gtest
// process accumulates per-run LLVM ORC session state, JIT code pages, and
// fragmented heap, producing heap-order-dependent crashes in mid-suite tests
// (codegen_env_disposer_672, codegen_sb_ctor_5, ...). The fix: each runJit
// call forks, the CHILD runs the full in-process pipeline above and _Exit()s
// (its entire heap + JIT code is reclaimed by the OS — the parent's RSS stays
// flat by copy-on-write), and the parent collects the result.
//
// The child hands the JitResult back through an anonymous temp file (unlinked
// on creation so it never leaks) using the small length-prefixed wire format
// below; its stderr is captured to a second anonymous temp file so ASan/LSan
// reports, ownership-check output, and program stderr still surface. A
// wall-clock deadline (BEZEL_JIT_TIMEOUT_SEC, default 60s) SIGKILLs a hung
// child so a single runaway test cannot wedge the suite — modelled on the
// #1624 cyclic-base-graph regression pin (commit 1509443). fork() failure
// (e.g. under the very memory pressure this fixes) degrades gracefully to
// in-process execution.
// ---------------------------------------------------------------------------

// Wire magic ("BZJR" little-endian) so a truncated/corrupt result record is
// distinguished from a real one rather than parsed as garbage.
constexpr std::int32_t kResultMagic = 0x524A425A;

static inline bool writeAll(int fd, const void* buf, std::size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Serialize a JitResult to `fd`. Same-machine only (native byte order).
static inline void serialize(int fd, const JitResult& r) {
    std::int32_t magic = kResultMagic;
    std::int32_t ran = r.ran ? 1 : 0;
    std::int32_t exitCode = r.exitCode;
    const auto& ds = r.diagnostics.diagnostics();
    std::int32_t ndiags = static_cast<std::int32_t>(ds.size());
    writeAll(fd, &magic, sizeof(magic));
    writeAll(fd, &ran, sizeof(ran));
    writeAll(fd, &exitCode, sizeof(exitCode));
    writeAll(fd, &ndiags, sizeof(ndiags));
    for (const auto& d : ds) {
        std::int32_t sev = static_cast<std::int32_t>(d.severity);
        std::uint32_t line = d.location.line, col = d.location.column,
                      off = d.location.offset;
        std::uint32_t mlen = static_cast<std::uint32_t>(d.message.size());
        writeAll(fd, &sev, sizeof(sev));
        writeAll(fd, &line, sizeof(line));
        writeAll(fd, &col, sizeof(col));
        writeAll(fd, &off, sizeof(off));
        writeAll(fd, &mlen, sizeof(mlen));
        if (mlen) writeAll(fd, d.message.data(), mlen);
    }
}

static inline bool readAll(int fd, void* buf, std::size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t rr = ::read(fd, p, n);
        if (rr < 0) { if (errno == EINTR) continue; return false; }
        if (rr == 0) return false;  // EOF before n bytes (short / truncated)
        p += rr; n -= static_cast<std::size_t>(rr);
    }
    return true;
}

// Parse a serialized JitResult from `fd` (positioned at 0). Returns false on
// any short/mismatched read; on success `out` is fully populated (diagnostics
// reconstructed via DiagnosticEngine::report so errorCount is consistent).
static inline bool parse(int fd, JitResult& out) {
    std::int32_t magic = 0;
    if (!readAll(fd, &magic, sizeof(magic)) || magic != kResultMagic)
        return false;
    std::int32_t ran = 0, exitCode = 0, ndiags = 0;
    if (!readAll(fd, &ran, sizeof(ran)) ||
        !readAll(fd, &exitCode, sizeof(exitCode)) ||
        !readAll(fd, &ndiags, sizeof(ndiags)))
        return false;
    out.ran = (ran != 0);
    out.exitCode = exitCode;
    for (std::int32_t i = 0; i < ndiags; ++i) {
        std::int32_t sev = 0;
        std::uint32_t line = 0, col = 0, off = 0, mlen = 0;
        if (!readAll(fd, &sev, sizeof(sev)) || !readAll(fd, &line, sizeof(line)) ||
            !readAll(fd, &col, sizeof(col)) || !readAll(fd, &off, sizeof(off)) ||
            !readAll(fd, &mlen, sizeof(mlen)))
            return false;
        if (mlen > (1u << 20)) return false;  // corrupt stream: refuse huge message
        std::string msg;
        if (mlen) {
            msg.resize(mlen);
            if (!readAll(fd, msg.data(), mlen)) return false;
        }
        SourceLocation loc;
        loc.line = line; loc.column = col; loc.offset = off;
        out.diagnostics.report(static_cast<Severity>(sev), loc, std::move(msg));
    }
    return true;
}

// Read the whole captured stderr stream (bounded so a runaway can't OOM us).
static inline std::string readStderr(int fd) {
    std::string out;
    char buf[4096];
    std::size_t cap = 1u << 18;  // 256 KiB ceiling
    for (;;) {
        ssize_t rr = ::read(fd, buf, sizeof(buf));
        if (rr < 0) { if (errno == EINTR) continue; break; }
        if (rr == 0) break;
        if (out.size() < cap) out.append(buf, static_cast<std::size_t>(rr));
    }
    return out;
}

}  // namespace jit_isolate

// Per-test wall-clock deadline (seconds). Legit JIT tests are sub-second; this
// only bounds a HUNG test (infinite loop / deadlock) so it can't wedge the
// suite, and is well under the ~300s per-shard timeout in run_parallel.sh. Set
// BEZEL_JIT_TIMEOUT_SEC to override; the deadline is disabled only if the env
// var is set to 0.
inline int jitTimeoutSec() {
    static const int v = [] {
        const char* e = std::getenv("BEZEL_JIT_TIMEOUT_SEC");
        if (!e || *e == '\0') return 60;
        int n = std::atoi(e);
        return n >= 0 ? n : 60;
    }();
    return v;
}

// Process-isolated runJit. Forks; the child runs runJitInProcess, serializes
// the JitResult, and _Exit()s (discarding its LLVM/heap state); the parent
// waitpids under the deadline, SIGKILLs on timeout, and reconstructs the
// JitResult. The contract (ran / exitCode / diagnostics) is identical to the
// old in-process path; only the heap lifetime changes. See namespace jit_isolate
// above for the full fork-safety + isolation rationale.
inline JitResult runJitWithOptions(std::string_view source,
                                   const codegen::CodegenOptions& opts) {
    ensureNativeTargetInitialized();

    char outTmpl[] = "/tmp/bezel_jit_outXXXXXX";
    char errTmpl[] = "/tmp/bezel_jit_errXXXXXX";
    int outFd = ::mkstemp(outTmpl);
    int errFd = ::mkstemp(errTmpl);
    if (outFd < 0 || errFd < 0) {
        // Cannot capture results across the fork boundary — degrade to the
        // in-process path so the test still runs.
        if (outFd >= 0) { ::close(outFd); ::unlink(outTmpl); }
        if (errFd >= 0) { ::close(errFd); ::unlink(errTmpl); }
        return runJitInProcess(source, opts);
    }
    // Unlink now: the fds stay valid (anonymous temp files, auto-reclaimed on
    // close) and no name ever leaks even if the parent is killed.
    ::unlink(outTmpl);
    ::unlink(errTmpl);

    pid_t pid = ::fork();
    if (pid < 0) {
        // fork() failed (e.g. under the memory pressure this fix targets, or
        // RLIMIT_NPROC) — degrade to in-process rather than failing the test.
        ::close(outFd);
        ::close(errFd);
        return runJitInProcess(source, opts);
    }

    if (pid == 0) {
        // ---- CHILD ----
        // Capture this run's stderr (ASan/LSan diagnostics, the ownership-check
        // block's ===OWNERSHIP_FAIL=== output, program stderr) into errFd.
        ::dup2(errFd, STDERR_FILENO);
        // Run the identical in-process pipeline, then hand the result to the
        // parent. Errors keep `ran` false and are carried in r.diagnostics.
        JitResult r = runJitInProcess(source, opts);
        jit_isolate::serialize(outFd, r);
        ::fsync(outFd);
        // _Exit (NOT exit): skip atexit handlers and static destructors in the
        // forked copy. The child's whole heap — LLVM allocators, JIT code pages,
        // the LLJIT/module trees — is reclaimed by the OS here, which is what
        // keeps the parent's RSS flat across the full suite. bz_flush_static_owned
        // already ran inside runJitInProcess (while the LLJIT was alive), and the
        // process-exit atexit path that would dereference dangling JIT vtables is
        // never reached. (Same discipline as the #1624 fork pin, commit 1509443.)
        std::_Exit(0);
    }

    // ---- PARENT ----
    const int timeout = jitTimeoutSec();
    int status = 0;
    bool timedOut = false;
    if (timeout == 0) {
        // Deadline disabled: block until the child exits.
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            break;  // ECHILD or similar; status left 0
        }
    } else {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout);
        for (;;) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) break;              // reaped
            if (w < 0) { if (errno == EINTR) continue; break; }
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (timedOut) {
            // SIGKILL is uncatchable; the blocking waitpid reaps the child.
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                break;
            }
        }
    }

    // Collect whatever the child wrote.
    ::lseek(outFd, 0, SEEK_SET);
    ::lseek(errFd, 0, SEEK_SET);
    JitResult r;
    bool parsed = jit_isolate::parse(outFd, r);
    std::string childStderr = jit_isolate::readStderr(errFd);
    ::close(outFd);
    ::close(errFd);

    // Helper: a failure record preserving ran=false and surfacing the captured
    // stderr (bounded) so the crash/timeout is diagnosable in the suite log.
    auto failWith = [&](std::string msg) {
        r.ran = false;
        r.exitCode = -1;
        if (!childStderr.empty()) {
            // Cap the embedded excerpt; the full text is also echoed below.
            std::string ex = childStderr.substr(
                0, std::min<std::size_t>(childStderr.size(), 8192));
            msg += "\n--- child stderr ---\n" + ex;
        }
        r.diagnostics.report(Severity::Error, SourceLocation{}, std::move(msg));
        return r;
    };

    if (timedOut) {
        return failWith("runJit child did not terminate within " +
                        std::to_string(timeout) + "s; killed (runaway/infinite "
                        "loop or deadlock)");
    }
    if (WIFSIGNALED(status)) {
        // Child crashed (e.g. SIGSEGV/SIGABRT from a double-free or failed
        // assertion in the JIT'd program, or killed). The in-process path
        // would have crashed the whole gtest process; isolating to the child
        // lets the suite continue and report ran=false for this test.
        return failWith("runJit child terminated by signal " +
                        std::to_string(WTERMSIG(status)));
    }
    if (!parsed) {
        // Child exited (normally or via _exit) without a complete result
        // record — e.g. it crashed between running the program and finishing
        // serialize(), or std::_Exit was reached with no record at all.
        int es = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return failWith("runJit child exited without a valid result record "
                        "(child exit status " + std::to_string(es) + ")");
    }

    // Normal completion: r carries the program's ran/exitCode/diagnostics.
    // Echo any captured stderr to the parent's stderr so ASan/LSan/program
    // output appears in the suite log exactly as the in-process path would
    // have shown it (the child's stderr was redirected to errFd, not the
    // parent's fd 2).
    if (!childStderr.empty()) std::fprintf(stderr, "%s", childStderr.c_str());
    return r;
}

// Convenience: run in unchecked (default release) mode.
inline JitResult runJit(std::string_view source) {
    return runJitWithOptions(source, codegen::CodegenOptions{});
}

// §28.2 v0.134 / issue #601: run in checked disposal mode. Equivalent to
// runJit but with checkedDisposal=true: instance-member entry guards are
// emitted, and DisposedAccessException is raised on use-after-dispose.
inline JitResult runJitChecked(std::string_view source) {
    codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    return runJitWithOptions(source, opts);
}

struct EmitResult {
    DiagnosticEngine diagnostics;
    bool lowered = false;       // true once a module was produced (no FE errors)
    bool verifies = false;      // true if llvm::verifyModule found NO errors
};

// Lex + parse + sema + lower, then run llvm::verifyModule on the emitted module
// WITHOUT requiring a `main` (so a declaration-only library fragment — e.g. a
// top-level `extension` block — can be checked). `verifies` is true only when
// the module is structurally valid IR. Used to assert that a function body
// emits well-typed IR (e.g. a generic-extension template stub returning the
// correct LLVM type, not `ret i32` into a `ptr` function — issue #699 / §26.4).
inline EmitResult emitAndVerify(std::string_view source) {
    ensureNativeTargetInitialized();
    EmitResult r;

    Lexer lexer(source, r.diagnostics);
    Parser parser(lexer, r.diagnostics);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return r;

    sema::SemaContext ctx(r.diagnostics);
    sema::collectSymbols(*unit, ctx);
    sema::resolveTypes(*unit, ctx);
    sema::typecheckExpressions(*unit, ctx);
    if (r.diagnostics.hasErrors()) return r;

    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = codegen::emitModule(*unit, ctx, *llvmCtx,
                                      codegen::CodegenOptions{});
    if (!module) return r;
    r.lowered = true;

    // llvm::verifyModule returns TRUE when the module is BROKEN; invert it.
    r.verifies = !llvm::verifyModule(*module, &llvm::errs());
    return r;
}

}  // namespace bezel::test
