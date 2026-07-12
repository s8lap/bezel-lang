#pragma once

// Canonical AOT test-harness helper — the JIT->AOT test-migration foundation
// (migration plan §2, P0). `runAot`/`runAotChecked` are drop-in counterparts to
// the in-process JIT `runJit`/`runJitChecked` (support/codegen_support.hpp):
// instead of ORC-JITting `main` in the test process, they compile the source to
// a NATIVE executable and run it in a subprocess — the same path `bezelc` ships,
// and therefore the stricter, correct oracle (it runs ownership + definite-
// assignment + static-init-cycle checks the JIT harness deliberately skips).
//
// CONSOLIDATION, NOT GREEN FIELD. This reuses the machinery that already exists
// instead of re-implementing it:
//   * runAot        -> bezel::Driver::compileFile (src/driver/Driver.cpp) — the
//                      real bezelc AOT path (full front-end -> emitObjectFile ->
//                      clang link against the plain libbezel_runtime.a, whose
//                      path is baked into bezel_core as BEZEL_RUNTIME_LIB_PATH).
//                      The link recipe is NOT re-written here.
//   * runAotChecked -> the same front-end in-process + emitObjectFile, linked
//                      -fsanitize=address against the memoized ASan runtime
//                      object from native_test_harness.hpp (asanRuntimeObject()),
//                      run under LeakSanitizer. This is the per-program memory
//                      net (WS2): a leak / double-free / use-after-free at exit
//                      directly verifies Bezel's dispose-exactly-once invariant.
//                      It generalizes the ~176 hand-rolled copies of this recipe
//                      (canonical: tests/codegen/generic_usingvar_disposal_test
//                      .cpp `runAotExit`).
//
// RETURN TYPE: both return `JitResult` (the runJit struct), so every downstream
// `.ran` / `.exitCode` / `.diagnostics` call site is untouched (migration plan
// §2; WS6 §3). The added `JitResult::stdoutText` carries the captured program
// stdout — the value channel for observables that cannot fit an 8-bit exit code.
//
// TWO SEMANTIC DIFFERENCES FROM runJit (documented, inherent — not bugs here):
//   1. runJit returns the raw 32-bit int from main(); a process exit status is
//      8-bit (WEXITSTATUS masks to 0..255). A test whose Main returns a value
//      outside 0..255 (256, 1559, -1, ...) does NOT round-trip through the exit
//      code and must move its observable to stdout (captured in stdoutText).
//   2. The AOT front-end is STRICTER: a program runJit accepts (it skips the
//      ownership / DA / static-init / §33.5 entry-point passes) may be REJECTED
//      here, surfacing as ran=false with the rejection in `diagnostics`.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/codegen_support.hpp"   // JitResult + the FE/codegen includes
#include "native_test_harness.hpp"       // asanRuntimeObject() (memoized)
#include "driver/Driver.hpp"             // Driver::compileFile (the AOT path)
#include "sema/DefiniteAssignment.hpp"   // checkDefiniteAssignment (checked leg)
#include "sema/StaticInitCycles.hpp"     // checkStaticInitCycles  (checked leg)

namespace bezel::test {

namespace aot_detail {

namespace fs = std::filesystem;

// Base scratch dir for per-run temp artifacts. Per CLAUDE.md: never /tmp (tmpfs
// is RAM; a scratch object tree there thrashes the box) — /var/tmp is disk.
inline std::string scratchBase() { return "/var/tmp/bezel-scratch/aot-harness"; }

// Per-test wall-clock deadline for the compiled program (seconds); mirrors
// codegen_support::jitTimeoutSec so a hung program cannot wedge the run.
// BEZEL_AOT_TIMEOUT_SEC overrides; 0 disables the deadline.
inline int aotTimeoutSec() {
    static const int v = [] {
        const char* e = std::getenv("BEZEL_AOT_TIMEOUT_SEC");
        if (!e || *e == '\0') return 60;
        int n = std::atoi(e);
        return n >= 0 ? n : 60;
    }();
    return v;
}

inline std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// fork/exec `argv` with fd 1 -> outPath and fd 2 -> errPath (both truncated),
// applying `extraEnv` (setenv in the child before exec — used for ASAN_OPTIONS).
// Blocks until the child exits or the deadline elapses; on timeout the child is
// SIGKILLed and reaped. Returns the raw waitpid status and sets *timedOut.
// timeoutSec <= 0 disables the deadline. Returns -1 if fork() itself fails.
inline int spawnCaptured(const std::vector<std::string>& argv,
                         const std::string& outPath, const std::string& errPath,
                         int timeoutSec, bool& timedOut,
                         const std::vector<std::pair<std::string, std::string>>&
                             extraEnv = {}) {
    timedOut = false;
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // ---- CHILD ----
        for (const auto& kv : extraEnv)
            ::setenv(kv.first.c_str(), kv.second.c_str(), /*overwrite=*/1);
        int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int errFd = errPath == outPath
                        ? outFd
                        : ::open(errPath.c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd >= 0) ::dup2(outFd, STDOUT_FILENO);
        if (errFd >= 0) ::dup2(errFd, STDERR_FILENO);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execv(cargv[0], cargv.data());
        ::_Exit(127);  // exec failed
    }
    // ---- PARENT ----
    int status = 0;
    if (timeoutSec <= 0) {
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        return status;
    }
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    for (;;) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) return status;                       // reaped
        if (w < 0) { if (errno == EINTR) continue; break; }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return status;
}

// A per-call working directory under scratchBase(). Empty string on failure.
inline std::string makeWorkDir() {
    std::error_code ec;
    fs::create_directories(scratchBase(), ec);
    std::string tmpl = scratchBase() + "/aotXXXXXX";
    std::vector<char> tbuf(tmpl.begin(), tmpl.end());
    tbuf.push_back('\0');
    const char* dirp = ::mkdtemp(tbuf.data());
    return dirp ? std::string(dirp) : std::string{};
}

inline bool keepArtifacts() { return std::getenv("BEZEL_AOT_KEEP") != nullptr; }

// RAII cleanup of a work dir (kept when BEZEL_AOT_KEEP is set, for debugging).
struct WorkDir {
    std::string path;
    ~WorkDir() {
        if (path.empty()) return;
        if (keepArtifacts()) {
            std::fprintf(stderr, "[aot] kept %s\n", path.c_str());
            return;
        }
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Redirect the process's fd 2 to `path` for the duration; restore on scope exit.
// Used to capture Driver::compileFile's diagnostics (it writes to std::cerr) AND
// the clang link subprocess's stderr (it inherits fd 2) into `diagnostics`.
struct StderrCapture {
    int saved = -1;
    explicit StderrCapture(const std::string& path) {
        std::fflush(stderr);
        saved = ::dup(STDERR_FILENO);
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
    }
    ~StderrCapture() {
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
    }
};

// True if `text` carries an ASan / LSan report headline — the runAotChecked
// "sanitizer fired" signal, independent of the exitcode=99 sentinel.
inline bool hasSanitizerReport(const std::string& text) {
    static const char* markers[] = {
        "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer", "detected memory leaks",
        "runtime error:",  // UBSan, if the runtime leg ever enables it
    };
    for (const char* m : markers)
        if (text.find(m) != std::string::npos) return true;
    return false;
}

}  // namespace aot_detail

// ---------------------------------------------------------------------------
// runAot — compile `source` to a native executable via the REAL bezelc driver
// (Driver::compileFile) and run it, capturing {ran, exitCode, stdout, diags}.
//
//   ran      : true iff it compiled, linked, and ran to normal termination.
//   exitCode : the process exit status (8-bit) when ran==true; -1 on a crash /
//              timeout (mirrors runJit's failWith).
//   stdoutText: the program's captured stdout.
//   diagnostics: on a compile/link reject, one Error diagnostic carrying the
//              driver's stderr (so `.hasErrors()` is true and downstream
//              wrappers that inspect diagnostics behave as under runJit).
// ---------------------------------------------------------------------------
inline JitResult runAot(std::string_view source,
                        const codegen::CodegenOptions& opts) {
    JitResult r;

    aot_detail::WorkDir wd{aot_detail::makeWorkDir()};
    if (wd.path.empty()) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAot: could not create work dir under " +
                                 aot_detail::scratchBase());
        return r;
    }
    namespace fs = aot_detail::fs;
    const std::string srcPath = (fs::path(wd.path) / "prog.bz").string();
    const std::string exePath = (fs::path(wd.path) / "prog").string();
    const std::string ccErr   = (fs::path(wd.path) / "cc.err").string();
    const std::string outPath = (fs::path(wd.path) / "out.txt").string();
    const std::string errPath = (fs::path(wd.path) / "run.err").string();

    {
        std::ofstream os(srcPath, std::ios::binary);
        os.write(source.data(), static_cast<std::streamsize>(source.size()));
    }

    // --- Compile + link via the real driver (its link recipe, not ours). -----
    CompileResult cc;
    {
        aot_detail::StderrCapture cap(ccErr);  // grab driver + clang stderr
        Driver driver;
        cc = driver.compileFile(srcPath, exePath, /*deps=*/{}, opts);
    }
    if (!cc.success) {
        std::string diag = aot_detail::slurp(ccErr);
        if (diag.empty()) diag = "runAot: bezelc rejected the program";
        r.diagnostics.report(Severity::Error, SourceLocation{}, std::move(diag));
        return r;  // ran stays false — AOT is the stricter oracle (see header)
    }

    // --- Run the executable, capturing stdout (stderr -> separate file). ------
    bool timedOut = false;
    int status = aot_detail::spawnCaptured({exePath}, outPath, errPath,
                                           aot_detail::aotTimeoutSec(), timedOut);
    r.stdoutText = aot_detail::slurp(outPath);

    if (timedOut) {
        r.exitCode = -1;
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAot: program did not terminate within " +
                                 std::to_string(aot_detail::aotTimeoutSec()) +
                                 "s; killed (runaway / deadlock)");
        return r;
    }
    if (WIFSIGNALED(status)) {
        r.exitCode = -1;  // mirror runJit's failWith
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAot: program terminated by signal " +
                                 std::to_string(WTERMSIG(status)));
        return r;
    }
    r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.ran = WIFEXITED(status);
    return r;
}

// Convenience: default (release) codegen options — full parity with runJit.
inline JitResult runAot(std::string_view source) {
    return runAot(source, codegen::CodegenOptions{});
}

// ---------------------------------------------------------------------------
// runAotChecked — the per-program AddressSanitizer/LeakSanitizer memory net
// (migration plan §2 memory tier; WS2). Same stricter front-end as runAot
// (ownership + DA + static-init), emitted to a native object, linked
// -fsanitize=address against the memoized ASan runtime object, and run under
// LeakSanitizer. A sanitizer error at exit (leak, double-free, use-after-free)
// FAILS: it surfaces as ran=false with the sanitizer report in `diagnostics`,
// so a wrapper doing `r.ran ? r.exitCode : -999` flips to the failure sentinel.
// A clean program returns normally (ran=true, exitCode=program's, LSan-clean).
//
// SKIP semantics: if the toolchain/runtime object is unavailable, returns
// ran=false with a diagnostic beginning "runAotChecked: SKIP" so a caller can
// distinguish "environment can't run this" from "the program is dirty".
// ---------------------------------------------------------------------------
inline JitResult runAotChecked(std::string_view source) {
    JitResult r;

    // The ASan-instrumented combined runtime object (built once, memoized,
    // shared across shards). Empty => toolchain unavailable => SKIP.
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: SKIP — ASan runtime object "
                             "unavailable (toolchain/build)");
        return r;
    }

    aot_detail::WorkDir wd{aot_detail::makeWorkDir()};
    if (wd.path.empty()) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: could not create work dir");
        return r;
    }
    namespace fs = aot_detail::fs;
    const std::string objPath = (fs::path(wd.path) / "prog.o").string();
    const std::string exePath = (fs::path(wd.path) / "prog").string();
    const std::string outPath = (fs::path(wd.path) / "out.txt").string();
    const std::string sanPath = (fs::path(wd.path) / "san.txt").string();

    // --- Full front-end IN-PROCESS (matches Driver's executable pipeline, so
    //     accept/reject parity with runAot holds) + emit the native object. ---
    {
        Lexer lexer(source, r.diagnostics);
        Parser parser(lexer, r.diagnostics);
        auto unit = parser.parseCompilationUnit();
        if (!unit || r.diagnostics.hasErrors()) return r;

        sema::SemaContext ctx(r.diagnostics);
        sema::collectSymbols(*unit, ctx);
        sema::resolveTypes(*unit, ctx);
        sema::typecheckExpressions(*unit, ctx);
        sema::checkDefiniteAssignment(*unit, ctx);
        sema::checkStaticInitCycles(*unit, ctx);
        ownership::checkOwnership(*unit, ctx);
        if (r.diagnostics.hasErrors()) return r;  // stricter oracle, ran=false

        llvm::LLVMContext llvmCtx;
        auto module = codegen::emitModule(*unit, ctx, llvmCtx);
        std::string err;
        if (!module || !codegen::emitObjectFile(*module, objPath, err)) {
            r.diagnostics.report(Severity::Error, SourceLocation{},
                                 "runAotChecked: codegen failed" +
                                     (err.empty() ? "" : ": " + err));
            return r;
        }
    }

    // --- Link -fsanitize=address against the ASan runtime object (WS2 §1). ----
    // Reuses native_test_harness's memoized runtime; the recipe mirrors the
    // ~176 hand-rolled `runAotExit`/`runUnderLeakSanitizer` copies verbatim.
    {
        std::string link = "clang++ -fsanitize=address -g " + objPath + " " +
                           rtObj + " -lm -lpthread -lstdc++ -o " + exePath +
                           " > /dev/null 2>&1";
        if (std::system(link.c_str()) != 0) {
            r.diagnostics.report(Severity::Error, SourceLocation{},
                                 "runAotChecked: ASan link failed");
            return r;
        }
    }

    // --- Run under LeakSanitizer. exitcode=99 sentinel distinguishes a
    //     sanitizer error from the program's own exit codes (WS2 §1). ---------
    bool timedOut = false;
    int status = aot_detail::spawnCaptured(
        {exePath}, outPath, sanPath, aot_detail::aotTimeoutSec(), timedOut,
        {{"ASAN_OPTIONS", "detect_leaks=1:exitcode=99:abort_on_error=0"}});
    r.stdoutText = aot_detail::slurp(outPath);
    std::string sanText = aot_detail::slurp(sanPath);

    if (timedOut) {
        r.exitCode = -1;
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: program timed out");
        return r;
    }

    const bool crashed = WIFSIGNALED(status);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const bool sanitizerFired =
        crashed || code == 99 || aot_detail::hasSanitizerReport(sanText);

    if (sanitizerFired) {
        // A leak / double-free / UAF is a dispose-exactly-once violation: FAIL.
        r.exitCode = crashed ? -1 : code;
        std::string msg = "runAotChecked: sanitizer fired (";
        msg += crashed ? "crash/abort" : "LSan/ASan at exit";
        msg += ")";
        if (!sanText.empty()) {
            const std::size_t cap = 8192;
            msg += "\n--- sanitizer report ---\n" +
                   sanText.substr(0, std::min(sanText.size(), cap));
        }
        r.diagnostics.report(Severity::Error, SourceLocation{}, std::move(msg));
        return r;
    }

    // Clean run: dispose-exactly-once verified for this program.
    r.exitCode = code;
    r.ran = WIFEXITED(status);
    return r;
}

}  // namespace bezel::test
