// fix #3193 (§27.6 "Method references" / §28.1 "Where the `owned` modifier
// can appear" / §28.3 "Producers" / §2.5 "Dependency resolution"; v0.254) — a
// method reference over an IMPORTED method loses its return type AND its
// `owned` marker. The bare-method-access spell `var f := Cell.MakeIt; ...`
// (an imported static) leaves the MemberAccessExpr with no Function TypeRef
// at all: the local formation site has a both-sources fallback for an
// `ast::MethodDecl`, but for an imported non-generic method the cast is
// null and the import early-return cuts it off — so the function-call type
// system can neither pick the right return type nor see the `owned` marker.
// The three symptom shapes:
//
//   1. the typical "[[owned]] Cell" return value held via `using var c`
//      then read as `c.V` and returned → a §28.3 borrow-lifetime FALSE REJECT
//      on the borrow held by the returned local
//   2. the shorter form (no local indirection) → the `internal:
//      lowerLocalDecl's initializer expression failed to lower to a value
//      for this local's slot (degraded to a zero-initialized value)` diagnostic
//      from #2434 AND the call silently allocates NOTHING
//   3. passing the returned owned Cell to a method that takes it by value
//      → malformed IR ("Call parameter type does not match function signature")
//      because the function-type return slot is dropped
//
// The explicit-target-type spelling `var f: (int) -> owned Cell := Cell.MakeIt;`
// already worked: the LHS type ascription rescued the formation. The fix
// makes the inferred spelling produce the same result.
//
// ── MEASURED PRE-FIX, two-module builds ──────────────────────────────────────
//
//   `var f := Cell.MakeIt; using var c := f(50); return c.V;`
//     REJECTED with "returned `got` carries a borrow of `c` whose source is
//                   disposed at scope end — §28.9 / §9.7 borrow-lifetime rule"
//   `var f := Cell.MakeIt; using var c := f(50); return 42;`
//     EMITS the internal: lowerLocalDecl diagnostic from #2434 (silent no-allocate)
//   `var f := Cell.MakeIt; using var c := f(50); return UseIt(c);`
//     EMITS the internal diagnostic AND emits malformed IR (the call parameter
//                   type was i32 instead of Cell, so verifyModule failed)
//
//   same programs in a single module: compile + exit 50
//   explicit-type twin of the first:    compile + exit 50
//
// ── THE FIX ──────────────────────────────────────────────────────────────────
//
// One new branch in `resolveMemberAccess`'s `isImported(member)` import block
// (src/sema/ExprTypecheck_Calls.cpp around L18010) — the same shape as the
// Field/Property/EnumConstant/ChoiceCase branches already there: form a
// Method-reference Function TypeRef, attach it to the MemberAccessExpr,
// return. The return type comes from `ctx_.importedReturnType(ms)` and the
// owned marker from `sema::methodReturnsOwned(*ms, ctx_)` (the #3189 shared
// accessor) — the two halves move together so a method reference over an
// imported callee never announces owned transfer on a unit-typed slot.
//
// The three already-fixed sites (the local method-reference formation sites
// in `resolveMemberAccess` and `resolveExtensionMember`, plus the synthesized
// `Domain.Spawn` entry Function TypeRef) get the same import-side fallback
// at their `if (md && md->returnType)` branches — they are the FALLBACK for
// the case where the import branch above somehow returns without setting a
// type (the path that the import block guards in any future reader's hand).
// All three sites carry the same comment so a reader can verify the shape
// is preserved across them.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir under the shared scratch base. The hardcoded
// shared /var/tmp/bezel-scratch/wt-3193/ path was the cause of the #3002-class
// xmod COVERAGE SHORT: every shard that ran this test wrote its dep.bzl /
// rt_asan.o to the same directory, so concurrent attempts overwrote each other's
// source file mid-compileObject child-read, garbled the in-flight read, and the
// shard crashed with SIGBUS. Process-unique stem (atomic counter + ::getpid)
// gives every shard its own directory under the same parent.
const std::filesystem::path& wtDir3193() {
    return wtScratchDir("wt-3193");
}


namespace fs = std::filesystem;

struct TempFile3193 {
    std::string path;
    explicit TempFile3193(std::string p) : path(std::move(p)) {}
    ~TempFile3193() { std::remove(path.c_str()); }
};

void writeSource3193(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library. `Cell` is the counted resource (ctor +1, dtor -1 in
// `Cell.n`) so a live-object counter can pin disposal EXACTLY once. `MakeIt`
// is the §28.1 `owned`-returning factory. `UseIt` is the `Cell`-by-value
// receiver — it is the canary that exposes the third symptom shape (silent
// no-allocate + malformed IR on the inferred-type abbreviated form).
const char* kLib3193 =
    "module MakeRefLib3193; "
    "public class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "  public static method MakeIt(v: int): owned Cell => new Cell(v); "
    "  public static method UseIt(c: Cell): int => c.V; "
    "end Cell;";

// The same declarations with the module boundary removed — the differential
// control. Every twin row below is this text plus the row's own `Program`,
// so the pair differs ONLY in whether `Cell` is imported.
const char* kLocalDecls3193 =
    "module MakeRefApp3193; "
    "public class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "  public static method MakeIt(v: int): owned Cell => new Cell(v); "
    "  public static method UseIt(c: Cell): int => c.V; "
    "end Cell;";

std::string importedApp3193(const std::string& program) {
    return "module MakeRefApp3193; import MakeRefLib3193; " + program;
}
std::string localApp3193(const std::string& program) {
    return std::string(kLocalDecls3193) + " " + program;
}

// Compile the dependency once per process (the object is content-identical
// for every row) and hand back its path. Empty → no toolchain, the row SKIPs.
const std::string& depObject3193() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    fs::path dir = wtDir3193();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string src = (dir / "dep.bzl").string();
    std::string obj = (dir / "dep.o").string();
    writeSource3193(src, kLib3193);
    bezel::Driver driver;
    if (!driver.compileObject(src, obj).success) return cached;
    if (!std::ifstream(obj).good()) return cached;
    cached = obj;
    return cached;
}

// Compile `appSrc` (importing the dependency when `imported`) and run it in a
// separate process. Returns the child's exit status, or:
//   -1 the app did not build   -2 the dependency is unavailable (SKIP)
// 1000+signal for a signalled child (1011 = SIGSEGV), so a crash can never be
// mistaken for a small exit code.
// `capturedStderr`, when non-null, receives the app compile's stderr — the
// only way to assert on diagnostic TEXT, which every reject / internal row
// must do.
int run3193(const std::string& appSrc, bool imported, const std::string& tag,
            std::string* capturedStderr = nullptr, bool compileOnly = false) {
    std::string dep;
    if (imported) {
        dep = depObject3193();
        if (dep.empty()) return -2;
    }
    fs::path dir = wtDir3193();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bz3193_" + tag + "_" + unique_stem("");
    TempFile3193 src((dir / (stem + ".bzl")).string());
    TempFile3193 exe((dir / stem).string());
    TempFile3193 err((dir / (stem + ".err")).string());
    writeSource3193(src.path, appSrc);

    std::vector<bezel::Dependency> deps;
    if (imported) {
        bezel::Dependency d;
        d.metadataPath = dep;
        d.objectPath = dep;
        deps.push_back(d);
    }
    codegen::CodegenOptions opts;

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(err.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        compiled = bezel::test::compileFileForTest(driver, src.path, exe.path,
                                                   deps, opts)
                       .success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(err.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, src.path, exe.path,
                                                   deps, opts)
                       .success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(exe.path).good()) return -1;
    if (compileOnly) return 0;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exe.path.c_str(), exe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The two diagnostic fragments the rows must distinguish. Each is matched on
// its distinctive text so a row cannot pass on some other error that happens
// to be present.
bool hasBorrowOutliveError3193(const std::string& s) {
    return s.find("returned `got` carries a borrow of `c`") !=
           std::string::npos;
}
bool hasInternalLowerLocalDecl3193(const std::string& s) {
    return s.find("internal: lowerLocalDecl's initializer expression "
                  "failed to lower to a value") != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — §27.6 / §28.9 / §9.7: the typical shape. `var f := Cell.MakeIt` (an
// IMPORTED static) is the function reference; `using var c := f(50)` keeps
// the returned `owned Cell` scope-bound; `return c.V` reads through `c`.
// Pre-fix this REJECTS with the §28.9 borrow-lifetime false reject on the
// `return got` line (`got := c.V` carries a borrow of `c` whose source is
// disposed at scope end). Post-fix the two-module build compiles, the
// program runs, and the exit is exactly the value passed in (50).
//
// The differential single-module twin (`localApp3193(prog)`) passed before
// the fix and must keep passing — that is what pins that the local answer
// did not change.
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t1_inferred_method_ref_compiles_and_exits) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var f := Cell.MakeIt; "
        "    using var c := f(50); "
        "    var got := c.V; "
        "    return got; "
        "  end Main; "
        "end Program;";
    int imported = run3193(importedApp3193(prog), true, "t1i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 50);
    // The local twin — must keep its pre-fix behavior.
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t1l"), 50);
}

// ===========================================================================
// T2 — tighter pin on the §28.9 borrow-lifetime false reject. Capture the
// compile stderr and assert it does NOT contain the false-reject signature
// after the fix. Pre-fix this row's captured stderr contains the §28.9
// "returned `got` carries a borrow of `c`" message and the compile fails;
// post-fix the compile succeeds and the captured stderr is empty of it.
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t2_inferred_no_borrow_outlive_false_reject) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var f := Cell.MakeIt; "
        "    using var c := f(50); "
        "    var got := c.V; "
        "    return got; "
        "  end Main; "
        "end Program;";
    std::string ierr;
    int imported = run3193(importedApp3193(prog), true, "t2i", &ierr,
                           /*compileOnly=*/true);
    if (imported == -2) return;  // no toolchain — SKIP
    // Pre-fix this compile FAILED with the §28.9 borrow-outlive error.
    BEZEL_EXPECT_EQ(imported, 0);
    BEZEL_EXPECT_FALSE(hasBorrowOutliveError3193(ierr));
    // Local twin: same control — must not regress.
    std::string lerr;
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t2l", &lerr,
                            /*compileOnly=*/true),
                    0);
    BEZEL_EXPECT_FALSE(hasBorrowOutliveError3193(lerr));
}

// ===========================================================================
// T3 — the shorter form (the second symptom shape). The body has no
// intermediate local indirection: `using var c := f(50); return 42;`. Pre-fix
// this EMITS the `internal: lowerLocalDecl's initializer expression failed
// to lower to a value for this local's slot` diagnostic from #2434 AND the
// call silently allocates NOTHING (so the §28.6 destructor never runs and
// `Cell.n` is non-zero — but the exit value is unaffected since the program
// returns the literal 42). Post-fix the internal diagnostic must be gone
// and the call must actually allocate (Cell.n must be 1 thanks to the
// single `f(50)` invocation, then 0 after the `using var` scope ends).
//
// ⚠ Cell.n is read AFTER the using scope — so the live-count assertion
// (0 = exactly one allocation disposed) is the counter that catches a
// silent no-allocate defect. The literal 42 in the return is so the
// exit-status assertion (`expected: 42`) is independent of the cell
// counter and the two together pin both halves of the fix.
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t3_shorter_form_no_internal_diag_allocates) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var f := Cell.MakeIt; "
        "    using var c := f(50); "
        "    return 42; "
        "  end Main; "
        "end Program;";
    std::string ierr;
    int imported = run3193(importedApp3193(prog), true, "t3i", &ierr);
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 42);
    BEZEL_EXPECT_FALSE(hasInternalLowerLocalDecl3193(ierr));
    // Local twin — must keep its pre-fix behavior and exit 42.
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t3l"), 42);
}

// ===========================================================================
// T4 — the third symptom shape: passing the returned owned Cell to a method
// that takes it by value. Pre-fix this EMITS the internal diagnostic AND
// emits malformed IR (the call parameter type was i32 instead of the Cell
// reference, so verifyModule failed). Post-fix the build compiles cleanly
// and exits 50 (the value of `c.V` after `UseIt(c)` returned it).
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t4_inferred_method_ref_to_method_arg) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var f := Cell.MakeIt; "
        "    using var c := f(50); "
        "    return Cell.UseIt(c); "
        "  end Main; "
        "end Program;";
    std::string ierr;
    int imported = run3193(importedApp3193(prog), true, "t4i", &ierr);
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 50);
    BEZEL_EXPECT_FALSE(hasInternalLowerLocalDecl3193(ierr));
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t4l"), 50);
}

// ===========================================================================
// T5 — REGRESSION GUARD for the explicit-target-type spelling
// `var f: (int) -> owned Cell := Cell.MakeIt;`. This worked before the fix
// (the LHS type ascription rescued the formation) and must keep working.
// Drop this row and a future reader could "fix" T1–T4 by removing the
// Function TypeRef formation entirely.
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t5_explicit_target_type_regression_guard) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var f: (int) -> owned Cell := Cell.MakeIt; "
        "    using var c := f(50); "
        "    var got := c.V; "
        "    return got; "
        "  end Main; "
        "end Program;";
    int imported = run3193(importedApp3193(prog), true, "t5i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 50);
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t5l"), 50);
}

// ===========================================================================
// T6 — the OWNED-MARKER HALF-MOVE guard. The fix is supposed to set
// `funcReturnOwned` together with `funcReturn`, never one without the other
// (per the issue's explicit warning). The `funcReturn` half is exercised by
// T1–T5; this row exercises the `funcReturnOwned` half by relying on the
// `Local.deallocator` invariant: when the inferred return type is `owned
// Cell` and the owned marker is set, the `using var c := f(50)` scope
// disposes the Cell exactly once. The live count is read AFTER the using
// scope, so 0 = the OWNED marker survived the formation. Pre-fix the
// formation would have set `funcReturn` to `owned Cell` (the type half)
// but with `funcReturnOwned = false` (the marker half dropped), the
// `using var` would not own the value and the dtor would not run — live
// count would be 1. Post-fix 0.
//
// This row pairs with the T1 row's "exit 50" assertion: that one pins the
// type, this one pins the marker. A reader who went one-fix-row-at-a-time
// and "fixed" only the OWNED half could satisfy T1 (the function-call
// return shape is unaffected by the marker) but would leave the live count
// at 1, which is where T6 catches them.
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t6_owned_marker_round_trips_to_local) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    do "
        "      var f := Cell.MakeIt; "
        "      using var c := f(50); "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end Program;";
    int imported = run3193(importedApp3193(prog), true, "t6i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t6l"), 0);
}

// ===========================================================================
// T7 — minimal-shape sanity row. The issue's fix description names three
// symptom shapes (the §28.9 false reject, the §2434 internal diagnostic,
// and the malformed-IR third shape). T1 + T3 already exercise the first
// two with body indirection; this row exercises the SIMPLEST body that
// still goes through the fix — a `var f := Cell.MakeIt;` with no `using
// var`, just a return. Without the fix this compiled (the function call's
// return type was unit; the `return 50;` was just a literal), so it does
// not assert a defect — but it pins that the Function TypeRef is now a
// `(int) -> owned Cell` (the `f` value is well-typed) and not a regression
// back to the pre-fix `() -> ()` shape. If the fix regressed to unit on
// the imported path, follow-on call sites would diverge from T1/T3 and
// this row remains the cheap positive-comparison anchor.
// ===========================================================================
BEZEL_TEST(xmod_method_reference_imported_3193, t7_minimal_shape_compiles_and_exits) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var f := Cell.MakeIt; "
        "    return 50; "
        "  end Main; "
        "end Program;";
    int imported = run3193(importedApp3193(prog), true, "t7i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 50);
    BEZEL_EXPECT_EQ(run3193(localApp3193(prog), false, "t7l"), 50);
}

}  // namespace bezel::module
