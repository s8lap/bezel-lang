// fix #3189 (§28.1 "Where the `owned` modifier can appear" / §28.3 "Producers"
// and "Unbound owned temporaries" / §21.9 "Expression statements" / §25.2
// "Operator overloading restrictions" / §28.14 "Scoped callback parameters" /
// §2.5 "Dependency resolution"; v0.254) — the `owned`-RETURN channel across a
// module boundary. Family M2 of umbrella #3152; see
// docs/proposals/member-symbol-audit-2026-08-13.md, §7 rank 6.
//
// ── WHAT THE DEFECT WAS ────────────────────────────────────────────────────
//
// "Does this call return `owned`?" is the sound discriminator between a fresh
// +1 the caller must dispose and a BORROW that reclaiming would
// use-after-free. Fifteen sites asked it, and every one asked it as
//
//     dynamic_cast<const ast::MethodDecl*>(ms->decl)->returnOwned
//
// `ModuleLoader` sets `MemberSymbol::decl` only where it re-parsed exported
// body text for a GENERIC type, so for a method of any NON-GENERIC imported
// class that cast is null and each site took its `false` branch — which is not
// "unknown" but the substantive claim "returns nothing owned". The fact was on
// the wire the whole time (`MetaMember::returnOwned` →
// `SemaContext::importedReturnOwned`); exactly three of the fifteen sites
// consulted it, and drift #5 of the audit is two arms of ONE lambda, thirteen
// lines apart, disagreeing about it.
//
// ── MEASURED PRE-FIX, real two-module builds ───────────────────────────────
//   `build/bezelc -c lib.bzl -o lib.o` then `build/bezelc --import lib.o app.bzl`,
//   each against a character-identical single-module twin:
//
//   bare-discard statement `Fac.Make(i);` x200   3200 bytes / 200 allocations
//                                                leaked (LSan); twin: clean
//   unbound receiver temp  `Fac.Make(i).v` x200  3200 bytes / 200 allocations
//                                                leaked (LSan); twin: clean
//   borrowing `arr.Add(Cell.MakeIt(i))` x50       672 bytes /  42 allocations
//   imported `operator +(): owned Cell`,         live-object count 2 (the
//     `using var r := a + b + c`                 intermediate AND the result);
//                                                twin: 0
//   `using var sub := imported.Subscribe(...)`   §28.14 FALSE REJECT (#3189);
//                                                twin: compiles
//
// ── WHY THE ROWS BELOW COUNT OBJECTS AND DO NOT JUST CHECK FOR A CRASH ─────
//
// Every leak row is paired with a LIVE-OBJECT COUNTER row: `Cell`'s constructor
// increments a process-global `n` and its destructor decrements it, so the
// value returned after the scope closes is 0 iff every allocation was disposed
// EXACTLY once — a leak reads > 0 and a double free reads < 0 or crashes. That
// matters here more than usual: this fix WIDENS a predicate that a dozen
// codegen disposal sites consume, and the failure mode of a bad widening is a
// double free, not a leak. The ASan rows are the second oracle, not the first;
// per #3176's header the dependency artifact itself cannot be instrumented
// (`--sanitize=address` is rejected together with `-c`), though LSan does see
// its allocations through the interposed allocator — which is how the byte
// counts above were obtained.
//
// ── THE OVER-REJECTION CONTROLS ────────────────────────────────────────────
//
// T5 pins that an imported BORROW-returning method is still NOT disposed (the
// use-after-free direction — the whole reason `returnOwned` is the gate), and
// T7 pins that §28.14 still REJECTS a capture-bearing closure at an imported
// callee whose return is unmarked. Without those two, this file would assert
// only that the compiler became more permissive.

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
// shared /var/tmp/bezel-scratch/wt-3189/ path was the cause of the #3002-class
// xmod COVERAGE SHORT: every shard that ran this test wrote its dep.bzl /
// rt_asan.o to the same directory, so concurrent attempts overwrote each other's
// source file mid-compileObject child-read, garbled the in-flight read, and the
// shard crashed with SIGBUS. Process-unique stem (atomic counter + ::getpid)
// gives every shard its own directory under the same parent.
const std::filesystem::path& wtDir3189() {
    return wtScratchDir("wt-3189");
}


namespace fs = std::filesystem;

struct TempFile3189 {
    std::string path;
    explicit TempFile3189(std::string p) : path(std::move(p)) {}
    ~TempFile3189() { std::remove(path.c_str()); }
};

void writeSource3189(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   Cell — the counted resource. `n` is the LIVE count (ctor +1, dtor -1), so
//          0 after a scope closes means "disposed exactly once"; `MakeIt` is
//          the `owned`-returning factory; `operator +` is the §25 owned-return
//          operator (§25.2 forbids modifiers on operator PARAMETERS, so the
//          return slot is an operator's only ownership annotation).
//   Holder — `Borrow()` returns the SAME object unmarked: a §28.6 borrow of an
//          `owned` field. It is the canary that this fix did not start
//          disposing borrows.
//   Sub / Pub — the §28.14 pair. `Subscribe` has the `scoped` formal AND the
//          `owned` return, so a call site can resolve bounding form 2;
//          `Lend` is the same shape with the return UNMARKED, which must keep
//          resolving no bound.
const char* kLib3189 =
    "module OwnRetLib3189; "
    "public class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "  public static method GetN(): int => Cell.n; "
    "  public static method MakeIt(v: int): owned Cell => new Cell(v); "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.V + b.V); "
    "end Cell; "
    "public class Holder "
    "  public owned C1: Cell; "
    "  public constructor() C1 := new Cell(7); end constructor; "
    "  public method Borrow(): Cell => C1; "
    "  destructor end destructor; "
    "end Holder; "
    "public class Sub "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Sub; "
    "public class Pub "
    "  public owned Held: Sub; "
    "  public constructor() Held := new Sub(); end constructor; "
    "  public method Subscribe(scoped h: () -> ()): owned Sub => new Sub(); "
    "  public method Lend(scoped h: () -> ()): Sub => Held; "
    "  destructor end destructor; "
    "end Pub;";

// The same declarations with the module boundary removed — the differential
// control. Every twin row below is this text plus the row's own `Program`, so
// the pair differs ONLY in whether `Cell` / `Holder` / `Pub` are imported.
const char* kLocalDecls3189 =
    "module OwnRetApp3189; "
    "public class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "  public static method GetN(): int => Cell.n; "
    "  public static method MakeIt(v: int): owned Cell => new Cell(v); "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.V + b.V); "
    "end Cell; "
    "public class Holder "
    "  public owned C1: Cell; "
    "  public constructor() C1 := new Cell(7); end constructor; "
    "  public method Borrow(): Cell => C1; "
    "  destructor end destructor; "
    "end Holder; "
    "public class Sub "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Sub; "
    "public class Pub "
    "  public owned Held: Sub; "
    "  public constructor() Held := new Sub(); end constructor; "
    "  public method Subscribe(scoped h: () -> ()): owned Sub => new Sub(); "
    "  public method Lend(scoped h: () -> ()): Sub => Held; "
    "  destructor end destructor; "
    "end Pub;";

std::string importedApp3189(const std::string& program) {
    return "module OwnRetApp3189; import OwnRetLib3189; " + program;
}
std::string localApp3189(const std::string& program) {
    return std::string(kLocalDecls3189) + " " + program;
}

// Compile the dependency once per process (the object is content-identical for
// every row) and hand back its path. Empty → no toolchain, the row SKIPs.
const std::string& depObject3189() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    fs::path dir = wtDir3189();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string src = (dir / "dep.bzl").string();
    std::string obj = (dir / "dep.o").string();
    writeSource3189(src, kLib3189);
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
// `capturedStderr`, when non-null, receives the app compile's stderr — the only
// way to assert on diagnostic TEXT, which every reject row must do.
// `compileOnly` returns 0 for a successful build without running the program.
int run3189(const std::string& appSrc, bool imported, const std::string& tag,
            std::string* capturedStderr = nullptr, bool asan = false,
            bool compileOnly = false) {
    std::string dep;
    if (imported) {
        dep = depObject3189();
        if (dep.empty()) return -2;
    }
    fs::path dir = wtDir3189();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bz3189_" + tag + "_" + unique_stem("");
    TempFile3189 src((dir / (stem + ".bzl")).string());
    TempFile3189 exe((dir / stem).string());
    TempFile3189 err((dir / (stem + ".err")).string());
    writeSource3189(src.path, appSrc);

    std::vector<bezel::Dependency> deps;
    if (imported) {
        bezel::Dependency d;
        d.metadataPath = dep;
        d.objectPath = dep;
        deps.push_back(d);
    }
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = asan;

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
    // A row whose subject is the COMPILE outcome stops here — see T6 for why
    // running that particular program would assert someone else's defect.
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

// §21.9's bare-discard diagnostic, and §28.14's capture-admission diagnostic —
// matched on their distinctive fragments so a row cannot pass on some other
// error that happens to be present.
bool hasDiscardError3189(const std::string& s) {
    return s.find("produces an owned value that requires disposal") !=
           std::string::npos;
}
bool hasScopedRejectError3189(const std::string& s) {
    return s.find("a capture-bearing closure is not admitted here") !=
           std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — §28.3 "Unbound owned temporaries": a call to an IMPORTED
// `owned`-returning factory used ONLY as a member-access RECEIVER. The
// temporary is owned by nothing and must be reclaimed at the end of the full
// expression. Pre-fix `isFreshOwnedClassProducer` (src/common/OwnershipFacts.cpp
// — THE shared core a dozen codegen disposal sites consume) answered "not a
// producer", so nothing was emitted and it leaked.
//
// The live count is read AFTER the loop: 0 = every one of the 7 temporaries
// disposed exactly once. Pre-fix this row returned 7.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t1_imported_receiver_temp_disposed_28_3) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var i: int := 0; var sum: int := 0; "
        "    while i < 7 do "
        "      sum := sum + Cell.MakeIt(i).V; "
        "      i := i + 1; "
        "    end while; "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t1i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);
    // The differential control: the same text without the module boundary.
    // It passed BEFORE the fix and must keep passing after — that is what
    // pins that the local answer did not change.
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t1l"), 0);
}

// ===========================================================================
// T1b — the ASan/LSan oracle for the same shape, loop-amplified to 200. A
// one-shot `Main` is not a reliable LSan oracle (a leftover register copy makes
// a leaked block read as merely reachable), so the loop is load-bearing.
// LeakSanitizer forces exit 1 on any leak, so `exit == 0` IS the assertion.
// Pre-fix: 3200 bytes leaked in 200 allocations.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t1b_imported_receiver_temp_lsan_28_3) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var i: int := 0; var sum: int := 0; "
        "    while i < 200 do "
        "      sum := sum + Cell.MakeIt(i).V; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t1bi", nullptr,
                           /*asan=*/true);
    if (imported == -2 || imported == -1) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);
    int local = run3189(localApp3189(prog), false, "t1bl", nullptr,
                        /*asan=*/true);
    if (local == -1) return;
    BEZEL_EXPECT_EQ(local, 0);
}

// ===========================================================================
// T2 — §21.9 "Expression statements": a bare-discard statement of an imported
// `owned`-returning call. `ExprTypecheck.cpp`'s §21.9 walk classified it "not
// owned-produced", so neither the §21.9 inspection nor the trivial-dispose
// reclaim it gates ever ran and the value leaked outright.
//
// Post-fix the statement is reclaimed exactly as `new ImportedCell(1);` as a
// bare statement already was (measured: that spelling ran the destructor and
// freed the block on this base, before this change) — the two arms of one rule
// now agree. ⚠ The §21.9 DIAGNOSTIC still does not fire here, and that is a
// SECOND, independent defect this fix does not close: the importer cannot see
// an imported class's destructor at all (`ModuleMetadataBuilder` skips
// `DestructorSymbol` and no wire bit replaces it), so
// `common::isNonTrivialDisposeClass` misclassifies EVERY imported class as
// trivial-dispose. That is reachable with no `owned` return anywhere in sight —
// a LOCAL class deriving from an imported base with a destructor gets the same
// wrong answer from `new Derived();` — so it is filed as #3191 rather than
// papered over here. T8 below is the control that §21.9 still rejects what it
// can see.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t2_imported_bare_discard_reclaimed_21_9) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 7 do "
        "      Cell.MakeIt(i); "
        "      i := i + 1; "
        "    end while; "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t2i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);  // pre-fix: 7 (all seven leaked)
}

// ===========================================================================
// T3 — §25 / §28.3: an IMPORTED `operator +` declared with an `owned` return.
// `a + b + c` produces TWO owned values (the intermediate `(a+b)` and the
// result). Sema's `producerState`, codegen's `unwindOwnedLocalClass` (a2),
// `usingVarOwnsClassObjectDynamically` (b0b), the argument-temp classifier and
// `isOwnedTempOperand`'s operator arm all reached the marker through
// `dynamic_cast<ast::OperatorDecl*>(ms->decl)` — null for an imported operator.
// Pre-fix live count: 2. The all-local twin has been 0 since #912.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t3_imported_owned_operator_chain_25) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    do "
        "      using var a := new Cell(1); "
        "      using var b := new Cell(2); "
        "      using var c := new Cell(3); "
        "      using var result := a + b + c; "
        "    end; "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t3i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);  // pre-fix: 2
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t3l"), 0);
}

// ===========================================================================
// T4 — §28.3 (#1881): a fresh imported `owned` producer passed to a BORROWING
// `Array<T>.Add`. The collection borrows, so the caller keeps the obligation
// and the boundary is the call's own end. This is the second, independent
// codegen consumer of `isFreshOwnedClassProducer` (LlvmBackend_Calls.cpp), so a
// fix that repaired only the receiver path would leave this row red.
// Pre-fix: 672 bytes leaked in 42 allocations under LSan.
//
// The array is never READ after the loop — per §28.3 the element the collection
// borrowed dies at the end of the adding expression, so reading it would be the
// language's own dangling-borrow case, not this fix's subject.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t4_imported_producer_into_borrowing_add_28_3) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    do "
        "      var arr: Array<Cell> := []; "
        "      var i: int := 0; "
        "      while i < 7 do "
        "        arr.Add(Cell.MakeIt(i)); "
        "        i := i + 1; "
        "      end while; "
        "    end; "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t4i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);  // pre-fix: 7
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t4l"), 0);
}

// ===========================================================================
// T5 — OVER-REJECTION / USE-AFTER-FREE CONTROL. `Holder.Borrow()` returns the
// receiver's `owned` field UNMARKED: a §28.6 borrow. Used in the very same
// unbound-receiver-temp position T1 exercises, it must NOT be disposed —
// reclaiming it would free a value the `Holder` still owns.
//
// The count is read while `h` is STILL LIVE, so the discriminator is sharp:
// 1 = the Holder's `C1` is intact; 0 would mean the borrow was wrongly
// disposed (and the `Holder`'s own destructor would then double-free it).
// This row passed before the fix and must keep passing — `returnOwned` being
// the gate is the entire reason the fix is sound.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t5_imported_borrow_return_not_disposed_28_6) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    var i: int := 0; var sum: int := 0; "
        "    while i < 7 do "
        "      sum := sum + h.Borrow().V; "
        "      i := i + 1; "
        "    end while; "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t5i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 1);
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t5l"), 1);
}

// ===========================================================================
// T6 — §28.14 bounding form 2: "a call whose return is `owned` AND is the
// initializer of a `using var` local → the returned object". `scopedBoundingLocal`
// (src/ownership/Ownership_Checks.cpp) read the marker through `ms->decl`, so
// for an imported callee `ownedReturn` was false, forms 2/3 could never
// resolve, the call fell through to forms 4/5 (the receiver `p` is a plain
// local — no bound), and the capture-bearing lambda was REJECTED.
//
// This is #3189's headline row and it became reachable only with #3176: until
// `scoped`-formal detection read the symbol, §28.14's admission never ran for
// an imported callee at all.
//
// ⚠ THE ROW ASSERTS THE COMPILE OUTCOME AND DELIBERATELY DOES NOT RUN THE
// PROGRAM. This shape leaks 56 bytes in 3 allocations under LSan — and it does
// so IDENTICALLY in both modules, on a single-module program with no imports
// at all: a plain `var` receiver of a form-2 `scoped` call is never reclaimed
// even though the compiler emits the §28.3 auto-drop warning saying it is
// (filed as #3195, with a minimal single-module repro). Running the program
// here would make this row fail under `BEZEL_AOT_FORCE_SANITIZE=address` while
// asserting a defect that is not #3189's — the subject here is a FALSE REJECT,
// which is settled entirely by whether the build succeeds. Upgrade this row to
// a live-count row once #3195 lands.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t6_imported_owned_return_bounds_capture_28_14) {
    const std::string prog =
        "public class Box "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class Program "
        "  public static method Touch(b: Box) "
        "    var n: int := b.v; "
        "  end Touch; "
        "  public static method Main(): int "
        "    using var b := new Box(3); "
        "    var p := new Pub(); "
        "    using var sub := p.Subscribe(lambda() => Touch(b)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    std::string ierr;
    int imported = run3189(importedApp3189(prog), true, "t6i", &ierr,
                           /*asan=*/false, /*compileOnly=*/true);
    if (imported == -2) return;  // no toolchain — SKIP
    // Pre-fix this compile FAILED with the §28.14 capture-admission error.
    BEZEL_EXPECT_FALSE(hasScopedRejectError3189(ierr));
    BEZEL_EXPECT_EQ(imported, 0);
    std::string lerr;
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t6l", &lerr,
                            /*asan=*/false, /*compileOnly=*/true),
                    0);
    BEZEL_EXPECT_FALSE(hasScopedRejectError3189(lerr));
}

// ===========================================================================
// T7 — OVER-REJECTION CONTROL FOR T6, in the other direction: the SAME call
// shape against `Lend`, whose return is UNMARKED. Form 2 must still not
// resolve, so the capture-bearing closure must still be rejected — identically
// in both modules. Without this row T6 would be satisfied by a fix that simply
// stopped checking §28.14 across the boundary.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t7_imported_unmarked_return_still_rejects_28_14) {
    const std::string prog =
        "public class Box "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class Program "
        "  public static method Touch(b: Box) "
        "    var n: int := b.v; "
        "  end Touch; "
        "  public static method Main(): int "
        "    using var b := new Box(3); "
        "    var p := new Pub(); "
        "    using var sub := p.Lend(lambda() => Touch(b)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    std::string ierr;
    int imported = run3189(importedApp3189(prog), true, "t7i", &ierr);
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, -1);  // compile REJECTED
    BEZEL_EXPECT_TRUE(hasScopedRejectError3189(ierr));
    std::string lerr;
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t7l", &lerr), -1);
    BEZEL_EXPECT_TRUE(hasScopedRejectError3189(lerr));
}

// ===========================================================================
// T8 — OVER-REJECTION CONTROL for §21.9: the single-module twin of T2 is a HARD
// ERROR (`Cell` declares a destructor, so it is non-trivial-dispose), and this
// fix must not have disturbed that. The row is here because T2 asserts the
// imported side is silently ACCEPTED — without T8 a reader could not tell
// whether §21.9 still rejects anything at all.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t8_local_bare_discard_still_rejected_21_9) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    Cell.MakeIt(1); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    std::string lerr;
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t8l", &lerr), -1);
    BEZEL_EXPECT_TRUE(hasDiscardError3189(lerr));
}

// ===========================================================================
// T9 — `using var x := <imported owned call>` disposes EXACTLY once. This path
// already had the imported fallback (#1415 / #707), so the row is a
// no-regression control on the two sites whose hand-rolled
// `(md && md->returnOwned) || (!md && ctx_.importedReturnOwned(ms))` this fix
// collapses into the shared accessor: a double free would read as a crash or a
// negative live count, a lost fallback as 7.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t9_imported_using_var_disposes_once_28_10) {
    const std::string prog =
        "public class Program "
        "  public static method Main(): int "
        "    do "
        "      var i: int := 0; "
        "      while i < 7 do "
        "        using var r := Cell.MakeIt(i); "
        "        i := i + 1; "
        "      end while; "
        "    end; "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int imported = run3189(importedApp3189(prog), true, "t9i");
    if (imported == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(imported, 0);
    BEZEL_EXPECT_EQ(run3189(localApp3189(prog), false, "t9l"), 0);
}

// ===========================================================================
// T10 — fix #3191 follow-up boundary control (§21.9 / §3.6 / §28.9, v0.254).
// The same imported class with a destructor is exercised through BOTH the CALL
// and the NEW path:
//   - as a CALL:   `Cell.MakeIt(i);`  — bare-discard RECLAIMS (T2 pins this)
//   - as a NEW:    `new Cell(1);`     — bare-discard is a §21.9 hard error
//                                    (pinned by xmod_destructor_carriage_3191.T2)
//
// The §21.9 walk reads `TypeSymbol::importedNonTrivialDispose` to fire the
// reject — a carrier that #3191 added — but ONLY on the NEW arm. The CALL
// arm is reclaimed via `disposeOwnedValue` per #3189's design intent. This
// row pins the boundary in a SINGLE test so a future reader cannot satisfy
// one side by accidently removing the gate in src/sema/ExprTypecheck.cpp.
// The `expected: -1 / discard-error` leg asserts the NEW arm fires; the
// `expected: 0` leg asserts the CALL arm reclaims. Both legs use the same
// library so a single regression in either direction fails the row.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_channel_3189, t10_new_vs_call_boundary_3191) {
    // NEW arm: `new Cell(1);` as a bare-discard statement of an imported
    // class with a destructor — the §21.9 hard error MUST fire. This is the
    // #3191 case the wire-side carrier exists to fix; it must stay
    // rejected even after this follow-up.
    const std::string progNew =
        "public class Program "
        "  public static method Main(): int "
        "    new Cell(1); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    std::string newErr;
    int newR = run3189(importedApp3189(progNew), true, "t10n", &newErr);
    if (newR == -2) return;  // no toolchain — SKIP
    BEZEL_EXPECT_EQ(newR, -1);
    BEZEL_EXPECT_TRUE(hasDiscardError3189(newErr));

    // CALL arm: `Cell.MakeIt(1);` as a bare-discard of an imported CALL
    // returning owned Cell — the bare-discard MUST reclaim. This is the
    // #3189 path; the #3191 carrier's NEW-only gate is what preserves it
    // on the imported side. count must be 0 (destructor ran).
    const std::string progCall =
        "public class Program "
        "  public static method Main(): int "
        "    Cell.MakeIt(1); "
        "    return Cell.GetN(); "
        "  end Main; "
        "end Program;";
    int callR = run3189(importedApp3189(progCall), true, "t10c");
    BEZEL_EXPECT_EQ(callR, 0);
}

}  // namespace bezel::module
