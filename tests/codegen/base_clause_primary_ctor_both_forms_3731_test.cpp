// Spec-derived regression test for issue #3731 — `emitImplicitBase`'s
// constructor SELECTION for the §22.10 inheritance-clause base call
// (`class Derived(a: int) : Base(a)`) when the base declares BOTH a §22.10
// primary constructor and an explicit convenience constructor.
//
// ── THE SHAPE ──────────────────────────────────────────────────────────────
//
//     class Base(bp: int)
//       public bx: int := 11;
//       public constructor() this(0); end constructor;
//     end Base;
//     class Derived(a: int): Base(a)
//     end Derived;
//
//     new Derived(7);   // the base clause `Base(a)` must bind to Base's
//                       // PRIMARY constructor (arity 1) -- Base's only
//                       // EXPLICIT constructor has arity 0 and does not
//                       // match these arguments.
//
// ── WHAT THE DEFECT WAS (measured on r12-cycle-1 @ 28e5b15, before this fix)
//
// `emitImplicitBase` (src/codegen/LlvmBackend_Setup.cpp) resolved the base
// call with `ctorByArity(b, n, nullptr)` — a raw arity scan over `b`'s
// `ConstructorSymbol`s ONLY. A §22.10 primary constructor has no
// `ConstructorSymbol` (SymbolCollection.cpp creates one only for an explicit
// `ast::ConstructorDecl`), so `ctorByArity(Base, 1, nullptr)` found nothing
// and control fell through to the very next fallback,
// `ctorByArity(Base, 0, nullptr)` — which DID find Base's explicit
// `constructor()` and called it with NO arguments at all, silently dropping
// the base-clause value the primary was supposed to receive. That explicit
// constructor's own `this(0)` chain then re-ran Base's primary-parameter
// storage with ITS OWN literal `0`, clobbering the `7` the `new` site's
// `prePopulateBasePositionalFields` had already stored a moment earlier.
//
// MEASURED (bezelc @ 28e5b15, pre-fix): `a=7 bp=0 bx=11` — `bp` wrong,
// `bx` right (Base's own field initializer still ran, via the
// wrongly-invoked explicit constructor's `this(0)` chain — only the VALUE
// bound to the primary was wrong, not whether steps 1/2 ran at all, which is
// what distinguishes this from #3713's construction-sequence gap).
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Resolve the base call with `ctorForArgs(b, td->primaryBaseCtorArgs)` — the
// SAME resolver `new` uses (`ctorForArgs(t, ne.args) == nullptr` is how a
// direct-`new` site recognizes "route to the primary", #3625/#3713) — rather
// than a second, weaker predicate. When it returns null AND `b` declares
// BOTH a primary constructor of arity `n` AND at least one explicit
// constructor (otherwise the pre-existing no-explicit-ctor fallback already
// handles it byte-identically), the base call routes to `b`'s PRIMARY: its
// positional-param fields are already populated by
// `prePopulateBasePositionalFields` at the construction entry point, so what
// remains is exactly §22.8 steps 1+2 for `b` itself, run unconditionally
// (LOCAL: `emitLocalBaseFieldInit`; IMPORTED: `bz$<Base>$initprimary`, #3713).
//
// per §22.8 step 1 "The base or chained constructor call", issue #3731
// per §22.10 (v0.286) — "This replaces an explicit `base(...)` call"
// per §2.8.3 rule 4 (v0.286) — the importer observes the same surface
// per #3713 (v0.286) — `bz$<Type>$initprimary`, reused unchanged here
// per #3162 — the base-clause-argument asymmetry family

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
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base — forked
// shards would otherwise race on one another's `.o` in /tmp (tmpfs, RAM).
const std::filesystem::path& wtDir3731() {
    return wtScratchDir("wt-3731-codegen");
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
    auto dir = wtDir3731();
    std::string stem = unique_stem("bezel_3731lib_");
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
    auto dir = wtDir3731();
    std::string stem = unique_stem("bezel_3731app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    // compileFileForTest, not a raw driver.compileFile call, so this runner
    // is reached by the gate's `--full` generated-code ASan sweep
    // (support/driver_run_support.hpp) like every other hand-rolled runner.
    bezel::CompileResult rb =
        bezel::test::compileFileForTest(driver, r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// The base hierarchy shared by the "distinct arities" rows: a primary
// constructor (arity 1, `bp`) AND an explicit constructor of a DIFFERENT
// arity (0) — the exact shape #3731 is about. `bx` pins that Base's own
// field initializer (§22.8 step 2) still ran, isolating the defect to the
// VALUE bound to `bp`, not whether construction ran at all (#3713's axis).
const char* kBaseBothForms =
    "public class Base(bp: int) "
    "  public bx: int := 11; "
    "  public constructor() "
    "    this(0); "
    "  end constructor; "
    "end Base; ";

const char* kDerivedShow =
    "public class Derived(a: int): Base(a) "
    "  public method Show() "
    "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
    "  end Show; "
    "end Derived; ";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — LOCAL, the headline repro. Base declares BOTH forms at DISTINCT
// arities (primary arity 1, explicit arity 0).
// FAIL-WITHOUT: prints "a=7 bp=0 bx=11" (bp dropped; bx unaffected).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3731
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           local_base_clause_binds_primary_distinct_arities_spec_22_8) {
    std::string src =
        std::string("module L3731a; ") + kBaseBothForms + kDerivedShow +
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
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=7 bp=7 bx=11") != std::string::npos);
    // The pre-fix line, excluded explicitly so a regression cannot be read as
    // "some other value happened to contain the expected substring".
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 bp=0 bx=11") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — IMPORTED base, LOCAL derived: `Base` (both forms) lives in a
// dependency; `Derived` is declared in the IMPORTER and is NOT itself
// exported/imported. This is the shape that exercises `emitImplicitBase`'s
// NEW imported-`b` arm (`bz$Base$initprimary`, reusing #3713's second entry
// point) — unlike a Derived-ALSO-imported shape, where `emitImplicitBase`
// runs entirely inside the EXPORTER's own compile (via `bz$Derived$init`)
// and never sees `ctx_.isImported(Base) == true` at all.
// FAIL-WITHOUT: prints "a=7 bp=0 bx=11" (identical failure mode to T1 — the
// defect is in `emitImplicitBase`'s selection, reached identically from
// either side of a module boundary since it is one function).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10 / §2.8.3 rule 4 / #3713, issue #3731
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           imported_base_both_forms_routes_through_initprimary_spec_3713) {
    std::string lib = std::string("module XL3731b; ") + kBaseBothForms;
    const std::string app =
        "module XM3731b; "
        "import XL3731b; "
        "class Derived(a: int): Base(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
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
    BEZEL_EXPECT_TRUE(out.find("a=7 bp=7 bx=11") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("a=7 bp=0 bx=11") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — OVER-ACCEPTANCE / REGRESSION CONTROL: base has a primary constructor
// and NO explicit constructor at all. This shape already worked pre-fix
// (the pre-existing `else if (!emitImportedBaseFieldInit(...))` fallback,
// gated on `hasAnyConstructor(b) == false` for the NEW `else if` branch this
// fix adds) and must stay byte-identical — the `ctorForArgs` swap must not
// perturb the no-explicit-ctor base at all.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1, issue #3731 (regression control)
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           control_primary_only_no_explicit_ctor_unchanged) {
    const std::string src =
        "module L3731c; "
        "class Base(bp: int) "
        "  public bx: int := 11; "
        "end Base; "
        "class Derived(a: int): Base(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
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
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=7 bp=7 bx=11") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — OVER-ACCEPTANCE / REGRESSION CONTROL: base has ONLY an explicit
// constructor matching the base clause's arity (NO primary constructor at
// all, `!hasPositionalFields`). This is the #929 shape `ctorForArgs`'s
// single-match fast path (`ctorArityCount == 1`) already handled correctly
// via `ctorByArity` before this fix; confirms the resolver swap keeps it.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / fix #929, issue #3731 (regression control)
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           control_explicit_ctor_only_no_primary_unchanged) {
    const std::string src =
        "module L3731d; "
        "class Base "
        "  public bp: int; "
        "  public constructor(v: int) "
        "    bp := v; "
        "  end constructor; "
        "end Base; "
        "class Derived(a: int): Base(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp}\"); "
        "  end Show; "
        "end Derived; "
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
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=7 bp=7") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T5 — THE "SAME ARITY" PIN (§6b): base declares a primary constructor AND
// an explicit constructor of the SAME arity as the base clause's argument
// count. Per §22.10 ("This replaces an explicit `base(...)` call" — the base
// call resolves like any other constructor call), the EXPLICIT constructor
// wins: `ctorForArgs` finds it directly (an exact arity+type match), so the
// new "route to primary" branch never fires. Distinguishable from routing to
// the primary because the explicit ctor multiplies its argument by 100
// before chaining to the primary.
// FAIL-WITHOUT is not meaningful here (this shape reached the SAME `if`
// branch before and after #3731, just via `ctorByArity` vs `ctorForArgs`) —
// its purpose is to PIN the resolution rule so a future change cannot
// silently flip it to "primary always wins ties".
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 "This replaces an explicit `base(...)` call", issue #3731
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           same_arity_explicit_ctor_wins_spec_22_10) {
    const std::string src =
        "module L3731e; "
        "class Base(bp: int) "
        "  public bx: int := 11; "
        "  public constructor(v: int) "
        "    this(v * 100); "
        "  end constructor; "
        "end Base; "
        "class Derived(a: int): Base(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
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
    // The explicit constructor ran (bp = 7*100 = 700), not the primary
    // directly (which would read bp = 7).
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 bp=700 bx=11") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bp=7 ") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T6 — MIXED-TYPE OVERLOAD RESOLUTION (§6b): base has NO primary constructor
// but TWO explicit constructors of the SAME arity (1), disambiguated only by
// argument TYPE (`int` vs `string`). `ctorByArity` (pre-fix) picked
// whichever ctor happened to be declared FIRST regardless of the caller's
// argument type; `ctorForArgs` resolves by type, exactly as `new` already
// does. Two derived classes exercise both arms of the overload set through
// the base clause so a wrong pick is directly observable (`tag` reads the
// WRONG branch's marker).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 "Optional parameters and named arguments" (overload resolution
// this reuses) / §22.8 step 1, issue #3731
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           mixed_type_overload_resolution_at_base_clause) {
    const std::string src =
        "module L3731f; "
        "class Base "
        "  public bp: int; "
        "  public tag: string; "
        "  public constructor(v: int) "
        "    bp := v; "
        "    tag := \"int\"; "
        "  end constructor; "
        "  public constructor(s: string) "
        "    bp := 0; "
        "    tag := s; "
        "  end constructor; "
        "end Base; "
        "class DerivedInt(a: int): Base(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} tag={tag}\"); "
        "  end Show; "
        "end DerivedInt; "
        "class DerivedStr(s: string): Base(s) "
        "  public method Show() "
        "    Console.WriteLine($\"s={s} bp={bp} tag={tag}\"); "
        "  end Show; "
        "end DerivedStr; "
        "class C "
        "  public static method Main(): int "
        "    using var d1 := new DerivedInt(7); "
        "    d1.Show(); "
        "    using var d2 := new DerivedStr(\"hi\"); "
        "    d2.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 bp=7 tag=int") != std::string::npos);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("s=hi bp=0 tag=hi") != std::string::npos);
    // The wrong-overload readings, excluded explicitly: a first-declared-
    // wins arity scan (the pre-fix `ctorByArity` behavior) would bind BOTH
    // derived classes to the `int` constructor, printing `tag=int` for the
    // string row too (with `bp` left at its garbage-coerced value) instead
    // of `tag=hi`.
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("s=hi bp=0 tag=int") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T7 — THE `owned` AXIS. DISPOSAL-ONCE under ASan/LSan, 200-iteration
// amplified. `Base` declares BOTH forms (primary `owned r: Res`, arity 1;
// explicit `constructor()`, arity 0), so `Derived(owned x: Res, a: int):
// Base(x)` reaches the NEW "route to primary" branch exactly as T1 does —
// this row is the ownership-safety twin of T1's value row. `x` is
// DERIVED's own captured `owned` primary parameter, forwarded verbatim as
// the base-clause argument (an IDENTIFIER, not a fresh `new` expression in
// the base clause itself — that shape hits an unrelated, pre-existing
// defect in `prePopulateBasePositionalFields` for reference-typed base-
// clause arguments, filed separately as #3742; out of scope for this
// `emitImplicitBase`-only lane).
//
// Two oracles: `made=200 live=0` (Res's own static counters) proves the
// value flowed through and was disposed exactly once per construction, and
// `runAotChecked` (ASan + LeakSanitizer on the generated code) independently
// fails the row through `r.ran` on a leak or a double free.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1-2 / §28.4 "owned fields ... auto-disposed with their
// container" / §22.10, issue #3731
BEZEL_TEST(base_clause_primary_ctor_both_forms_3731,
           owned_base_clause_arg_disposed_exactly_once_asan_x200) {
    const std::string src =
        "module L3731g; "
        "class Res "
        "  public static _live: int := 0; "
        "  public static _made: int := 0; "
        "  public constructor() "
        "    _live := _live + 1; "
        "    _made := _made + 1; "
        "  end constructor; "
        "  destructor "
        "    _live := _live - 1; "
        "  end destructor; "
        "end Res; "
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor() "
        "    this(new Res()); "
        "  end constructor; "
        "end Base; "
        "class Derived(owned x: Res, a: int): Base(x) "
        "  public method Tag(): int "
        "    return a; "
        "  end Tag; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(new Res(), 5); "
        "      _ := d.Tag(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    // `ran == false` is BOTH the ASan/LSan failure signal and the
    // "environment cannot build an ASan executable" SKIP — assert it so
    // neither passes silently.
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0") == std::string::npos);
}

}  // namespace bezel::codegen
