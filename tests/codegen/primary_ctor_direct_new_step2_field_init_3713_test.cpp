// Spec-derived regression test for issue #3713 — §22.8 step 1 + step 2 on the
// §22.10 DIRECT-PRIMARY `new` shape, in BOTH of `lowerNew`'s arms.
//
// ── THE SHAPE ──────────────────────────────────────────────────────────────
//
// A class that declares BOTH a §22.10 primary constructor and an explicit
// convenience constructor, constructed through the PRIMARY because no explicit
// constructor matches the call's arity:
//
//     class WithCounter(a: int, b: string)
//       public x: int := WithCounter.Bump();
//       public constructor(x: int)  this(x, "conv");  end constructor;
//     end WithCounter;
//
//     using var t := new WithCounter(7, "direct");   // binds to the PRIMARY
//
// #3625's sema half (`checkNewExprArgs`'s fall-through to
// `checkPositionalNewArgsFromSymbols`) admitted this call; before it, the
// construction was rejected outright, so no codegen path had ever had to run
// §22.8's construction sequence for it.
//
// ── WHAT THE DEFECT WAS (measured on r12-cycle-1 @ 714a07b) ────────────────
//
// LOCAL arm (`!ctx_.isImported(t)`): step 0 ran (`storePrimaryCtorPositional
// Args`) and `emitInstanceInit` installed the vtable — and that was all.
// `ctorForArgs(t, ne.args)` returned null (the primary has no
// `ConstructorSymbol`; only the arity-1 convenience ctor does), so the
// explicit-ctor branch never fired, and `hasAnyConstructor(t)` excluded the
// `else if (!hasAnyConstructor(t))` branch that owns `emitImplicitBase`
// (step 1), `emitOwnFieldInit` (step 2), `emitNewPendingCheck` and the §22.16
// construction-exit invariant check. NONE of them ran.
//
// IMPORTED arm: step 0 ran, then `bz$<Type>$init` — which
// `emitInstanceInitFunction` deliberately keeps VTABLE-ONLY for a class that
// declares an explicit constructor, on the assumption that the importer also
// calls `bz$<Type>$ctor` (whose compiled body runs steps 1+2 internally). On
// this shape no `bz$<Type>$ctor` call is ever emitted, so the assumption is
// false and steps 1+2 ran nowhere on the exporter side either.
//
// MEASURED, identically on both sides (`bezelc` @ 714a07b):
//
//     new Derived(7) against  class Base(bp: int) { bx := 11 }
//                             class Derived(a: int): Base(a)
//                               { x := 22; noinit: int; property P := 33;
//                                 constructor() this(0); }
//
//     one module : a=7 x=0  bx=0  bp=7 noinit=0 P=0     <- WRONG
//     two modules: a=7 x=0  bx=0  bp=0 noinit=0 P=0     <- WRONG, and DIFFERENTLY
//     §22.8      : a=7 x=22 bx=11 bp=7 noinit=0 P=33
//
// Both sides were broken; on the field-initializer axes (`x`, `P`, `bx`) they
// were broken IDENTICALLY, so §2.8.3 rule 4 held there and repairing only the
// imported arm would have BROKEN it in the opposite direction — the importer
// producing a value its own single-module twin does not. On the base-clause
// parameter `bp` they were ALREADY divergent (7 locally, 0 across a module:
// the `new` site's `prePopulateBasePositionalFields` fills the base's
// positional slot from `TypeDecl::primaryBaseCtorArgs`, which is AST-only and
// does not cross a module boundary — the #3162 asymmetry), so that axis was a
// live §2.8.3 rule 4 violation of its own. Both are closed by the same change,
// and T1/T2 assert the SAME line on both arms, which is what pins it.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// One predicate, `directPrimaryNew` (`LlvmBackend_Exprs2.cpp`, computed once
// before the arm cascade so the two arms cannot disagree):
//     Class && hasPositionalFields && hasAnyConstructor && !ctorForArgs(...)
//
//   * LOCAL arm: widen the existing `else if (!hasAnyConstructor(t))` branch to
//     `|| directPrimaryNew` — reusing the ONE step-2 emission site rather than
//     adding a second.
//   * IMPORTED arm: call a SECOND exported entry point,
//     `bz$<Type>$initprimary(ptr this)` — §22.8 steps 1+2 with no vtable store
//     (`bz$<Type>$init` already made it). The importer cannot run step 2
//     itself: `emitOwnFieldInit` walks initializers off each member's `decl`,
//     always null for an imported symbol (#2374), and an initializer may name
//     the exporter's PRIVATE statics — the exact conclusion #2452 reached for
//     an imported no-explicit-ctor BASE.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// A STATIC COUNTER, not a field value. `x == 0` is ambiguous (a field of a
// defaultable type may legitimately read its §7.9 default); a process-global
// counter that never incremented is unambiguous proof the initializer
// EXPRESSION was never evaluated — and a counter reading exactly `1` proves
// §22.8's "field initializers run exactly once per instance construction",
// which is the failure mode on the OTHER side of the fix (a double run).
// Every row therefore asserts a value AND excludes both `0` and `2`.
//
// The `owned`-field row additionally runs under `runAotChecked`
// (ASan + LeakSanitizer on the generated program), amplified to 200
// constructions: a one-shot leak row passes on both sides of a fix because the
// allocation is still reachable at exit.
//
// per §22.8 (v0.286) — "Field initializers run as part of construction in
//   textual declaration order", steps 0-3, and "Field initializers run exactly
//   once per instance construction"
// per §22.10 (v0.286) — primary constructors
// per §2.8.3 rule 4 (v0.286) — the importer observes the same surface
// issue #3713

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base — the fork'd
// shards would otherwise race on one another's `.o` in /tmp (tmpfs, RAM).
const std::filesystem::path& wtDir3713() {
    return wtScratchDir("wt-3713-codegen");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// The dependency object. Its `TempFile` MUST outlive the dependent compile —
// `Driver::compileFile` reopens the `.o` to read its `.bezel.meta` section.
struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::string& libSrc) {
    LibHandle h;
    auto dir = wtDir3713();
    std::string stem = unique_stem("bezel_3713lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

AppHandle buildApp(const std::string& libObjPath, const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    auto dir = wtDir3713();
    std::string stem = unique_stem("bezel_3713app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// The §22.8 axis grid, as ONE class hierarchy, so a single printed line answers
// all five axes at once:
//   (i)   `a`   — a primary-constructor PARAMETER field   (step 0)
//   (ii)  `x`   — a field with an initializer             (step 2)
//   (iii) `noinit` — a field with NO initializer          (§7.9 default; must
//                                                          stay 0)
//   (iv)  `bx` / `bp` — the BASE subobject's own initializer and its
//                       inheritance-clause primary parameter   (step 1)
//   (v)   `P`   — an AUTO-PROPERTY initializer (§23.1)    (step 2)
// The `owned` axis is measured separately, below, because its oracle is
// disposal-once rather than a value.
const char* kGridBody =
    "class Base(bp: int) "
    "  public bx: int := 11; "
    "end Base; "
    "class Derived(a: int): Base(a) "
    "  public x: int := 22; "
    "  public noinit: int; "
    "  public property P: int := 33; "
    "  public constructor() "
    "    this(0); "
    "  end constructor; "
    "  public method Show() "
    "    Console.WriteLine($\"a={a} x={x} bx={bx} bp={bp} noinit={noinit} P={P}\"); "
    "  end Show; "
    "end Derived; ";

// The §22.8 answer for `new Derived(7)`. Pre-fix BOTH sides printed
// "a=7 x=0 bx=0 bp=0 noinit=0 P=0".
const char* kGridExpected = "a=7 x=22 bx=11 bp=7 noinit=0 P=33";

// The counter class + the has-both-constructors class the value rows use.
// `Bump()` is a static METHOD call in a field initializer — §22.8 admits
// "static members of any type" in an initializer, and the counter it moves is
// what makes "the initializer never evaluated" distinguishable from "the field
// happens to read its default".
const char* kCounterBody =
    "class WithCounter(a: int, b: string) "
    "  private static _bumps: int := 0; "
    "  public x: int := WithCounter.Bump(); "
    "  public constructor(x: int) "
    "    this(x, \"conv\"); "
    "  end constructor; "
    "  public static method Bump(): int "
    "    _bumps := _bumps + 1; "
    "    return _bumps; "
    "  end Bump; "
    "  public static method Bumps(): int "
    "    return _bumps; "
    "  end Bumps; "
    "  public method Show() "
    "    Console.WriteLine($\"a={a} b={b} x={x} bumps={WithCounter.Bumps()}\"); "
    "  end Show; "
    "end WithCounter; ";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — LOCAL arm, the headline. One module, no `import` anywhere.
// FAIL-WITHOUT: prints "a=7 x=0 bx=0 bp=0 noinit=0 P=0".
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 steps 1-2 / §22.10, issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           local_direct_primary_runs_steps_1_and_2_spec_22_8) {
    std::string src = std::string("module L3713a; ") + kGridBody +
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(kGridExpected) != std::string::npos);
    // The two pre-fix lines, excluded explicitly so a regression cannot be read
    // as "some other value happened to contain the expected substring". They
    // are DIFFERENT on the two arms and both are asserted absent on both, so
    // neither arm can regress into the other's wrong answer either:
    //   local  pre-fix: a=7 x=0 bx=0 bp=7 noinit=0 P=0   (`bp` DID land — the
    //                   `new` site's `prePopulateBasePositionalFields` ran)
    //   import pre-fix: a=7 x=0 bx=0 bp=0 noinit=0 P=0   (`bp` did NOT — the
    //                   base's positional slot is the exporter's to fill)
    // That difference is itself a §2.8.3 rule 4 violation this fix closes; see
    // the header block.
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 x=0 bx=0 bp=7 noinit=0 P=0") ==
        std::string::npos);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 x=0 bx=0 bp=0 noinit=0 P=0") ==
        std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — IMPORTED arm, the same declarations across a module boundary. The
// assertion is the LOCAL twin's exact output (§2.8.3 rule 4), not a separately
// reasoned expectation.
// FAIL-WITHOUT: prints "a=7 x=0 bx=0 bp=0 noinit=0 P=0".
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 steps 1-2 / §22.10 / §2.8.3 rule 4, issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           imported_direct_primary_matches_local_twin_spec_2_8_3) {
    std::string lib = std::string("module XL3713b; ") +
        "public class Base(bp: int) "
        "  public bx: int := 11; "
        "end Base; "
        "public class Derived(a: int): Base(a) "
        "  public x: int := 22; "
        "  public noinit: int; "
        "  public property P: int := 33; "
        "  public constructor() "
        "    this(0); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} x={x} bx={bx} bp={bp} noinit={noinit} P={P}\"); "
        "  end Show; "
        "end Derived; ";
    const std::string app =
        "module XM3713b; "
        "import XL3713b; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find(kGridExpected) != std::string::npos);
    // Both pre-fix lines (see T1 for why they differ), asserted absent here too.
    BEZEL_EXPECT_TRUE(
        out.find("a=7 x=0 bx=0 bp=0 noinit=0 P=0") == std::string::npos);
    BEZEL_EXPECT_TRUE(
        out.find("a=7 x=0 bx=0 bp=7 noinit=0 P=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — LOCAL, the counter oracle: the initializer ran EXACTLY once.
// FAIL-WITHOUT: "a=7 b=direct x=0 bumps=0".
// A double run (the failure mode on the other side of the fix) would print
// "x=2 bumps=2" and is excluded explicitly.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 "Field initializers run exactly once per instance construction"
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           local_direct_primary_runs_initializer_exactly_once_spec_22_8) {
    std::string src = std::string("module L3713c; ") + kCounterBody +
        "class C "
        "  public static method Main(): int "
        "    using var t := new WithCounter(7, \"direct\"); "
        "    t.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 b=direct x=1 bumps=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("x=0 bumps=0") == std::string::npos);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("x=2 bumps=2") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — OVER-ACCEPTANCE CONTROL, local: the CONVENIENCE-constructor call shape
// (`new WithCounter(6)`, arity 1, binds to the explicit ctor). It already ran
// step 2 inside the compiled constructor body pre-fix, so this row must be
// bit-identical before and after — it is the "did you double-run it?" pin.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 "For a `this(...)` chain, the chained-to constructor runs to
// completion", issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           convenience_ctor_path_unchanged_no_double_run) {
    std::string src = std::string("module L3713d; ") + kCounterBody +
        "class C "
        "  public static method Main(): int "
        "    using var t := new WithCounter(6); "
        "    t.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=6 b=conv x=1 bumps=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bumps=2") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T5 — OVER-ACCEPTANCE CONTROL, imported: the same convenience shape across a
// module boundary. The imported arm gained a call to `bz$<Type>$initprimary`;
// this row pins that it is NOT emitted when an explicit constructor matched,
// because `bz$<Type>$ctor`'s own body already ran steps 1+2 — a double call
// would print `bumps=2`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 / §2.8.3 rule 4, issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           imported_convenience_ctor_path_unchanged_no_double_run) {
    std::string lib = std::string("module XL3713e; public ") + kCounterBody;
    const std::string app =
        "module XM3713e; "
        "import XL3713e; "
        "class C "
        "  public static method Main(): int "
        "    using var t := new WithCounter(6); "
        "    t.Show(); "
        "    using var u := new WithCounter(9, \"direct\"); "
        "    u.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    // Both shapes in ONE program: the convenience call bumps the counter to 1,
    // the direct-primary call bumps it to 2. Each ran its initializer exactly
    // once, and the two paths agree on how many times.
    BEZEL_EXPECT_TRUE(out.find("a=6 b=conv x=1 bumps=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("a=9 b=direct x=2 bumps=2") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T6 — OVER-ACCEPTANCE CONTROL: a primary-constructor class with NO explicit
// constructor. It already reached the `else if (!hasAnyConstructor(t))` branch
// pre-fix; `directPrimaryNew`'s `hasAnyConstructor(t)` conjunct is what keeps
// it from being admitted a SECOND time. A regression that drops that conjunct
// prints `bumps=2`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 "Field initializers run exactly once per instance construction"
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           no_explicit_ctor_primary_class_unchanged_no_double_run) {
    const std::string src =
        "module L3713f; "
        "class P(a: int) "
        "  private static _n: int := 0; "
        "  public x: int := P.Bump(); "
        "  public static method Bump(): int "
        "    _n := _n + 1; "
        "    return _n; "
        "  end Bump; "
        "  public static method N(): int "
        "    return _n; "
        "  end N; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} x={x} n={P.N()}\"); "
        "  end Show; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new P(4); "
        "    p.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=4 x=1 n=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=2") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T7 — OVER-ACCEPTANCE CONTROL: constructor SELECTION is unchanged. One class,
// both call shapes: `new K(1, 2)` must still bind to the explicit arity-2
// constructor (which chains `this(p + q)`, so `a = 3`), and only `new K(9)`
// takes the new direct-primary path. Each construction runs the initializer
// once, so the counter reads 1 then 2.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §22.7 / §22.10, issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           explicit_ctor_arity_selection_unchanged) {
    const std::string src =
        "module L3713g; "
        "class K(a: int) "
        "  private static _n: int := 0; "
        "  public x: int := K.Bump(); "
        "  public constructor(p: int, q: int) "
        "    this(p + q); "
        "  end constructor; "
        "  public static method Bump(): int "
        "    _n := _n + 1; "
        "    return _n; "
        "  end Bump; "
        "  public static method N(): int "
        "    return _n; "
        "  end N; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} x={x} n={K.N()}\"); "
        "  end Show; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    using var k1 := new K(1, 2); "
        "    k1.Show(); "
        "    using var k2 := new K(9); "
        "    k2.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=3 x=1 n=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=9 x=2 n=2") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T8 — THE `owned` AXIS. DISPOSAL-ONCE under ASan/LSan, 200-iteration
// amplified.
//
// Giving a previously-inert slot a real value makes every consumer newly
// reachable (lane rule §7): pre-fix an `owned` field whose initializer is a
// `new` was NEVER ALLOCATED on this path, so there was nothing to dispose and
// no leak to find. Post-fix it is allocated on every construction, and the
// class disposer must free it exactly once.
//
// Two oracles in one program, because neither alone is sufficient:
//   * the counters — `made=200` proves the initializer ran on every one of the
//     200 constructions (pre-fix: `made=0`), and `live=0` proves the destructor
//     ran for every one of them;
//   * `runAotChecked` — ASan + LeakSanitizer on the GENERATED code, so a leak
//     (never freed) or a double free (freed twice) fails the row through
//     `r.ran`, independently of what the program prints.
//
// LOOP-AMPLIFIED to 200: a one-shot leak row passes on BOTH sides of a fix,
// because a single allocation is still reachable from a live slot at exit.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 2 / §28.4 "owned fields ... auto-disposed with their
// container" / §28.5, issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           owned_field_initializer_disposed_exactly_once_asan_x200) {
    const std::string src =
        "module L3713h; "
        "class Res "
        "  private static _live: int := 0; "
        "  private static _made: int := 0; "
        "  public constructor() "
        "    _live := _live + 1; "
        "    _made := _made + 1; "
        "  end constructor; "
        "  destructor "
        "    _live := _live - 1; "
        "  end destructor; "
        "  public static method Live(): int "
        "    return _live; "
        "  end Live; "
        "  public static method Made(): int "
        "    return _made; "
        "  end Made; "
        "end Res; "
        "class Holder(a: int) "
        "  public owned r: Res := new Res(); "
        "  public constructor() "
        "    this(0); "
        "  end constructor; "
        "  public method Tag(): int "
        "    return a; "
        "  end Tag; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var h := new Holder(5); "
        "      _ := h.Tag(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res.Made()} live={Res.Live()}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    // `ran == false` is BOTH the ASan/LSan failure signal and the "environment
    // cannot build an ASan executable" SKIP — assert it so neither passes
    // silently.
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Pre-fix: "made=0 live=0" (the initializer never ran, so there was
    // nothing to leak — which is exactly why the counter, not ASan alone, is
    // what makes this row able to fail).
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T9 — the `owned` axis ACROSS a module boundary, same 200-iteration
// amplification. The counter oracle carries it here: the cross-module rows
// build through `Driver` + a linked `.o` rather than `runAotChecked`'s
// single-source pipeline, so `made=200 live=0` is what proves the imported
// `bz$<Type>$initprimary` allocated the field on every construction and the
// class disposer released each one exactly once.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 2 / §28.4 / §2.8.3 rule 4, issue #3713
BEZEL_TEST(primary_ctor_direct_new_step2_field_init_3713,
           imported_owned_field_initializer_disposed_exactly_once_x200) {
    const std::string lib =
        "module XL3713i; "
        "public class Res "
        "  private static _live: int := 0; "
        "  private static _made: int := 0; "
        "  public constructor() "
        "    _live := _live + 1; "
        "    _made := _made + 1; "
        "  end constructor; "
        "  destructor "
        "    _live := _live - 1; "
        "  end destructor; "
        "  public static method Live(): int "
        "    return _live; "
        "  end Live; "
        "  public static method Made(): int "
        "    return _made; "
        "  end Made; "
        "end Res; "
        "public class Holder(a: int) "
        "  public owned r: Res := new Res(); "
        "  public constructor() "
        "    this(0); "
        "  end constructor; "
        "  public method Tag(): int "
        "    return a; "
        "  end Tag; "
        "end Holder; ";
    const std::string app =
        "module XM3713i; "
        "import XL3713i; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var h := new Holder(5); "
        "      _ := h.Tag(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res.Made()} live={Res.Live()}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("made=0") == std::string::npos);
}

}  // namespace bezel::codegen
