// fix #2687 (a) (§22.3 "Parameter modifiers" / §28.14 "Scoped callback
// parameters"; v0.278) — a `scoped x` by-value argument to an IMPORTED
// constructor's `scoped` parameter.
//
// (Revision note: the spec text in §22.3's `scoped` paragraph and §28.14
// has been stable across v0.237..v0.278; the citation is to the current
// revision the harness was pointed at.)
//
// WHAT THE DEFECT WAS — codegen-side only.
//
// §22.3 / #2659 already fixed the ownership half of a ConstructorSymbol
// call for an IMPORTED callee: `MethodOwnership::resolveCtorParams` (now
// `CalleeFormals`, per #3176) reads formals off the resolved
// `sema::ConstructorSymbol`, so an imported `owned` formal is SEEN as an
// owned sink — pre-fix it was the same null that gated the codegen side,
// and every imported formal was invisible. `MethodOwnership::scopedFormalAt`'s
// NewExpr arm (since #3176) does the same for `scoped`, so the freeze
// interval is statically enforced.
//
// The codegen half of #2659 admitted a by-value `owned` argument against
// an `owned` formal in `lowerNew`'s imported-branch `allPositional`
// predicate; #2659 explicitly closed `scoped` out with "it is a different
// (lifetime, not transfer) obligation and is filed, not guessed at". That
// filing is the (a) half of #2687.
//
// The `allPositional` predicate treats a non-empty, non-by-ref modifier
// as "not positional", so a `scoped` argument fell through to the bare
// `return obj` branch and tripped #2658's loudness guard. The program
// COMPILED — the loudness is a WARNING, not an error — but the imported
// `bz$T$ctor` was never emitted, the constructor body never ran, and
// every field it assigned kept its default. A degraded program that
// compiled.
//
// WHAT THE FIX ADDS — one further admission to the same `allPositional`
// predicate, in the same one-line idiom `#2659` already used for `owned`:
// `(a.modifier == "scoped" && importedFormalMod(ai) == "scoped")`. Lowering
// is identical to a plain positional argument: `scoped` is by VALUE
// (`isByRefMod("scoped")` is false — LlvmBackend_impl.hpp), the loop
// below takes its `lowerExpr` path, and the pointer the callee stores in
// a non-`owned` field of the constructed instance is the same value a
// borrow would yield. There is no ownership transfer, so no
// `nullTransferredOwnedLocalSlot` call at the "call about to run" point
// is needed (the `owned` half's bookkeeping is keyed on `a.modifier ==
// "owned" || importedFormalMod(ai) == "owned"`, neither of which is true
// here). The freeze obligation is enforced statically by
// `MethodOwnership::scopedFormalAt`'s NewExpr arm, which #3176 widened
// to the resolved `CalleeFormals` of an imported ConstructorSymbol —
// there is no runtime freeze-call this predicate has to guard.
//
// TEST DESIGN — what makes the test a *spec-legal* shape, not a degraded
// one.
//
// §28.14 (v0.180) is explicit: a `scoped p: T` parameter (where T is a
// class or interface) is a borrow, and the callee MAY store the borrow
// in a direct non-`owned` field of the bounding object (`this`); the
// callee may NOT read `p` as a value. So a test ctor body of the shape
// `out_ := p.x + 70` (directly dereferencing the scoped formal) trips
// §28.14 — *not* the defect this commit fixes. The correct shape is:
//
//     public class ScopedCtor
//       public stored_: Holder;     // non-owned field; stores the borrow
//       public constructor(scoped p: Holder)
//         stored_ := p;             // legal: store the borrow
//       end;
//     end ScopedCtor;
//
// and the caller reads through `w.stored_.x + 70`. The ctor body stores
// (legal); the caller uses the borrowed object as a borrow (legal); the
// freeze on the caller's `h` lasts until the bounding object `w` goes
// out of scope.
//
// Without the fix, the imported `bz$ScopedCtor$ctor` is never emitted,
// `stored_` keeps its null default, the read through `w.stored_.x` hits
// §22.9's null deref (AddressSanitizer / SIGSEGV) — the program crashes.
// With the fix, the ctor runs, `stored_` holds the borrow, and the read
// returns the value.
//
// EXPECTED EXIT. Post-fix: the read yields `1 + 70 = 71`. Pre-fix: SIGSEGV
// (1011) — the imported ctor was never emitted; `stored_` is null; the
// dereference faults. Both fit comfortably under runAot's 8-bit exit
// truncation and the AOT harness's signal-discriminated status code.
//
// TEST ROWS.
//
//   * T1 — the marked `new ScopedCtor(scoped h)` builds, the imported
//     ctor body runs, the field stored through the borrow is observed.
//     Pre-fix: SIGSEGV (1011) on the dereference.
//
//   * T2 — the SAME-MODULE control: byte-identical code without `import`
//     must produce the same 71 — pinning that the fix converged the two
//     paths rather than changing the local one.
//
// (Earlier draft T2/T3 ("missing `scoped` marker on a borrow" /
// "`scoped` marker on a borrow formal" as rejected shapes) were dropped:
// §22.3 mandates `M(scoped x)` but the missing-marker report does not
// exist in the current ownership pass — that gate is unenforced, so a
// test asserting it would assert behavior the compiler never held. The
// distinguished spelling is the marked one — the unmarked spelling
// compiles (with the `scoped` modifier on the formal) and behaves as a
// borrow. Testing what the compiler actually checks is the disciplined
// shape.)

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2687 {
    std::string path;
    explicit TempFile2687(std::string p) : path(std::move(p)) {}
    ~TempFile2687() { std::remove(path.c_str()); }
};

void writeSource2687(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   Holder   — a class with one field `x`, set in its constructor.
//   ScopedCtor — the imported class with `scoped p: Holder` ctor. The body
//                legally STORES the borrow in `stored_` per §28.14 (it
//                may not READ `p` as a value). If the ctor body never
//                ran, `stored_` stays null and the dereference faults.
//   PlainCtor — the SAME shape with a borrow (no `scoped`) formal — a
//                control for the `scoped`-marker predicates.
const char* kLib2687 =
    "module ScopedCtorLib2687; "
    "public class Holder "
    "  public x: int; "
    "  public constructor() x := 1; end; "
    "end Holder; "
    "public class ScopedCtor "
    "  public stored_: Holder; "
    "  public constructor(scoped p: Holder) "
    "    stored_ := p; "
    "  end; "
    "end ScopedCtor; "
    "public class PlainCtor "
    "  public stored_: Holder; "
    "  public constructor(p: Holder) "
    "    stored_ := p; "
    "  end; "
    "end PlainCtor;";

std::string appSource2687(const std::string& body, const std::string& tag) {
    return "module ScopedCtorApp2687" + tag + "; import ScopedCtorLib2687; "
           "class C " + body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a
// separate process. Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
// `capturedStderr`, when non-null, receives the app compile's stderr.
int runImported2687(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2687_" + tag + "_" + unique_stem("");

    TempFile2687 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2687 aObj((dir / (stem + "_a.o")).string());
    writeSource2687(aSrc.path, kLib2687);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2687 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2687 bExe((dir / (stem + "_b")).string());
    TempFile2687 bErr((dir / (stem + "_b.err")).string());
    writeSource2687(bSrc.path, appSource2687(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);  // 1011 = SIGSEGV
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Body shared by T1: build the holder, hand it to the imported ctor with
// `scoped`, read `stored_.x` (the borrow through the field), add 70,
// return it. The ctor body ran iff the read returned the value rather
// than SIGSEGV'ing; the choice of `+ 70` proves the BODY RAN (not just a
// 1 that could be default-init).
//
// `using var w` is required by §28.14 form 1 — the bounding object of a
// `scoped` admission to a ctor call is the constructed instance, and the
// freeze on `h` is enforced until THAT bound's scope exits; the
// `scopedInitOfUsingLocal_` registry that resolves the bound is populated
// only by `using`/`await using` initializers (Ownership_Checks.cpp:13062).
// A plain `var` initializer would yield `ScopedBound::Kind::None` and
// trip the "capture-bearing closure is not admitted here" report at
// line 13600. This is the exact shape stdlib §2.21 / Guide 1 §10
// (the `using var sub := …; evt.Subscribe(sub)` family) teaches.
std::string workBody2687(const std::string& newExpr) {
    return "  static method Work(): int "
           "    var h := new Holder(); "
           "    using var w := " + newExpr + "; "
           "    var q := w.stored_.x + 70; "
           "    return q; "
           "  end Work; "
           "  public static method Main(): int "
           "    return C.Work(); "
           "  end Main;";
}

}  // namespace

// ===========================================================================
// T1 — §22.3 / §28.14: the marked `new ScopedCtor(scoped h)` compiles, the
// imported constructor body runs, the field stored through the borrow is
// observed by the caller.
//
// Pre-fix: the imported `bz$ScopedCtor$ctor` is never emitted, `stored_`
// stays null, the dereference SIGSEGVs (status 1011). The fix removes no
// rejection and adds no runtime disposition — it changes which LLVM path
// the codegen takes, so the ctor body actually runs.
// ===========================================================================
BEZEL_TEST(xmod_ctor_scoped_arg_2687, t1_marked_scoped_arg_runs_ctor_body) {
    int rc = runImported2687(workBody2687("new ScopedCtor(scoped h)"), "t1");
    if (rc == -2) return;  // no toolchain
    BEZEL_EXPECT_EQ(rc, 71);
}

// ===========================================================================
// T2 — SAME-MODULE control: byte-identical code without `import` must
// produce the same 71. This is what made the defect a cross-module one
// rather than a §22.3 enforcement bug, and it pins that the fix
// converged the two paths rather than changing the local one.
// ===========================================================================
BEZEL_TEST(xmod_ctor_scoped_arg_2687, t2_same_module_control) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2687_t4_");
    TempFile2687 src((dir / (stem + ".bzl")).string());
    TempFile2687 exe((dir / stem).string());
    writeSource2687(
        src.path,
        std::string("module ScopedCtorCtl2687; ") +
            "public class Holder "
            "  public x: int; "
            "  public constructor() x := 1; end; "
            "end Holder; "
            "public class ScopedCtor "
            "  public stored_: Holder; "
            "  public constructor(scoped p: Holder) "
            "    stored_ := p; "
            "  end; "
            "end ScopedCtor; "
            "class C " + workBody2687("new ScopedCtor(scoped h)") + " end C;");

    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path, {}).success) {
        BEZEL_EXPECT_TRUE(false);
        return;
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exe.path.c_str(), exe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        // Captured here purely so the discriminator is reachable from the
        // failure-mode diagnostic on regression; the FAIL row asserts the
        // success path.
        BEZEL_EXPECT_TRUE(false);
        return;
    }
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    if (WIFEXITED(status)) BEZEL_EXPECT_EQ(WEXITSTATUS(status), 71);
}

}  // namespace bezel::module
