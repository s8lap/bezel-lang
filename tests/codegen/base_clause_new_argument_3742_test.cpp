// Spec-derived regression test for issue #3742 — a §22.10 inheritance-clause
// base-call argument that CARRIES A TYPE NODE (`: Base(new Res(a))`) was never
// evaluated, and the base's positional slot was read back as a wild pointer.
//
// ── THE SHAPE ──────────────────────────────────────────────────────────────
//
//     class Base(owned r: Res)
//       public method Use(): int  return r.Id();  end Use;
//     end Base;
//     class Derived(a: int): Base(new Res(a))
//     end Derived;
//
//     using var d := new Derived(5);   // -> SEGV inside Base.Use
//
// ── WHAT THE DEFECT ACTUALLY WAS (measured on r12-cycle-1 @ f517ddb) ───────
//
// NOT what the issue supposed. #3742 was filed as "a class-typed (reference)
// argument ... is never evaluated", pointing at `prePopulateBasePositional
// Fields` / `coerceNumericToType` for pointer-typed values. Measured on
// f517ddb, a class-typed base-clause argument produced by a METHOD CALL works
// perfectly (`: Base(Fact.Make(a))` -> `use=5 made=1`, row T7 below), and so
// do arithmetic, literal and interpolated-string arguments. The discriminator
// is not the argument's TYPE at all: it is whether the argument expression
// contains an `ast::Type` NODE.
//
// `Resolver::resolveTypeDecl` (src/sema/TypeResolution_Names.cpp) resolved the
// header's positional-field TYPES, the generic constraints, the choice-case
// payloads and every MEMBER — but never `TypeDecl::primaryBaseCtorArgs`. An
// `ast::Type` nested inside an expression is resolved ONLY by
// `resolveTypesInExpr` (which is exactly why issue #163 added that call for a
// field initializer, its comment naming `new T()` as the reason). So `Res`
// stayed unresolved; `ExprTC`'s `NewExpr` arm READS `ctx_.resolvedType(*ne->
// type)` and therefore attached no exprType; and `Emitter::lowerNew` returns
// nullptr the instant that read is null. `prePopulateBasePositionalFields`
// took its `if (!v) { baseArgVals.clear(); break; }` arm and the whole
// pre-population was abandoned — silently, with no diagnostic.
//
// MEASURED, f517ddb:
//   `: Base(new Res(a))`, `r` body-referenced      -> SEGV (slot read as ptr)
//   `: Base(new Res())`, §11.1 ResourceHolder      -> `made=0 live=0`
//                                                     (the `new` NEVER RAN)
//   `: Base(a)` / `Base(a+1)` / `Base("hi")` /
//   `Base($"v={a}")` / `Base(Fact.Make(a))`        -> all correct
//   `class Holder(a: int, owned r: Res := new Res(77))` -> SEGV (the SIBLING
//                                                     header expression
//                                                     position, same gap)
//
// ── THE SECOND HALF: EXACTLY ONCE (§22.8 step 1) ──────────────────────────
//
// Making the argument evaluable exposed a second, pre-existing defect that is
// harmless for a literal and a leak for a `new`. TWO emitters walk
// `primaryBaseCtorArgs` for one construction — `prePopulateBasePositional
// Fields` (stores them into the base's positional slots) and `emitImplicitBase`
// (passes them to the base's EXPLICIT constructor when one resolves) — and both
// LOWERED them. MEASURED on f517ddb, before any part of this fix:
//
//     class Base(bp: int) { bx := 11; constructor(v: int) this(v * 100); }
//     class Derived(a: int): Base(Counter.Next())
//     -> `a=7 bp=200 bx=11 n=2`     §22.8 step 1 requires `bp=100 ... n=1`
//
// §22.8 step 1 is "The base or chained constructor call" — one call, one
// evaluation of its arguments. The fix states the "will `emitImplicitBase`
// lower and call these?" question ONCE (`Emitter::baseClauseExplicitCtor`) and
// has both sides read it, so the pre-population declines to evaluate exactly
// when the base constructor is going to. That constructor's own §22.8 step 0
// writes the same slots (§22.10 requires every convenience constructor of a
// primary-constructor class to chain via `this(...)`).
//
// per §22.10 "When a class with a primary constructor has a base class, the
//   base call appears in the inheritance clause" (v0.286)
// per §22.8 step 1 "The base or chained constructor call" (v0.286)
// per §22.8 step 0 / §22.10 "Capture rule and layout stability" (v0.286)
// per §11.1 ResourceHolder — an `owned` parameter consumed by an initializer
// per §22.5 "Optional parameters and named arguments" — the default twin (T13)
// per §2.8.3 rule 4 — the importer observes the same surface (T11/T12)
// issue #3742; siblings #3731 (emitImplicitBase selection), #3713, #3162

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
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
const std::filesystem::path& wtDir3742() {
    return wtScratchDir("wt-3742-codegen");
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
    auto dir = wtDir3742();
    std::string stem = unique_stem("bezel_3742lib_");
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
    auto dir = wtDir3742();
    std::string stem = unique_stem("bezel_3742app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    // compileFileForTest, not a raw driver.compileFile call, so this runner is
    // reached by the gate's `--full` generated-code ASan sweep
    // (support/driver_run_support.hpp) like every other hand-rolled runner.
    bezel::CompileResult rb =
        bezel::test::compileFileForTest(driver, r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// `Res` carries BOTH static counters, which is the only oracle that separates
// "the argument expression never ran" (`made=0`) from "it ran and the value
// was mis-stored" and from "it ran TWICE" (`made` double the loop count).
// A value read alone cannot tell those apart.
const char* kRes =
    "class Res "
    "  public static _live: int := 0; "
    "  public static _made: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) "
    "    _id := id; "
    "    _live := _live + 1; "
    "    _made := _made + 1; "
    "  end constructor; "
    "  destructor "
    "    _live := _live - 1; "
    "  end destructor; "
    "  public method Id(): int => _id; "
    "end Res; ";

// The exported twin for the two-module rows. Spelled out rather than derived
// from `kRes` by string surgery so that a reader can see exactly what the
// dependency declares.
const char* kResPublic =
    "public class Res "
    "  public static _live: int := 0; "
    "  public static _made: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) "
    "    _id := id; "
    "    _live := _live + 1; "
    "    _made := _made + 1; "
    "  end constructor; "
    "  destructor "
    "    _live := _live - 1; "
    "  end destructor; "
    "  public method Id(): int => _id; "
    "end Res; ";

// The §22.10 body-position capture shape: `r` is referenced in a body, so it
// is captured as a compiler-generated `owned` private field and disposed by
// the generated `Dispose()` (§22.10 "Capture rule and layout stability",
// §28.4). That is what makes the `new` in the base clause OWNED CORRECTLY
// rather than leaked, so `runAotChecked` can be a real oracle here.
const char* kBaseOwnedBody =
    "class Base(owned r: Res) "
    "  public method Use(): int "
    "    return r.Id(); "
    "  end Use; "
    "end Base; ";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — THE HEADLINE REPRO, value oracle. A `new` expression written directly
// in the inheritance clause must be evaluated exactly once and transferred
// into the base's positional parameter.
// FAIL-WITHOUT: SIGSEGV inside `Base.Use` (the slot holds the calloc zero and
// is dereferenced as a pointer) — measured on f517ddb.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 (base call in the inheritance clause) / §22.8 step 1, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           local_new_expression_base_clause_argument_spec_22_10) {
    const std::string src =
        std::string("module L3742a; ") + kRes + kBaseOwnedBody +
        "class Derived(a: int): Base(new Res(a)) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(5); "
        "    Console.WriteLine($\"use={d.Use()} made={Res._made}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // `made=1` is the exactly-once half; `use=5` is the transferred-value half.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("use=5 made=1") != std::string::npos);
    // Pre-fix the program crashed, so no line at all; a REGRESSION to double
    // evaluation would read `made=2`. Exclude it explicitly.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=2") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — THE SAME SHAPE UNDER ASan/LSan, 200-iteration amplified. A one-shot
// leak row passes on BOTH sides (the allocation is still reachable at exit),
// so the loop is what makes this an oracle: `made=200 live=0` proves each
// base-clause `new` ran exactly once per construction AND was disposed
// exactly once, and `runAotChecked` independently fails on a leak/double-free.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 "Capture rule and layout stability" / §28.4, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           local_new_base_clause_arg_disposed_exactly_once_asan_x200) {
    const std::string src =
        std::string("module L3742b; ") + kRes + kBaseOwnedBody +
        "class Derived(a: int): Base(new Res(a)) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(i); "
        "      _ := d.Use(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
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
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=400") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — §11.1's ResourceHolder shape: the `owned` primary parameter is
// consumed by an OWNED FIELD INITIALIZER and therefore NOT captured
// (§22.10 "Referenced only in field initializers ... not captured"). This is
// the row whose pre-fix output was the unambiguous proof of NON-EVALUATION —
// `made=0` cannot be explained by a mis-stored value.
// FAIL-WITHOUT: `made=0 live=0` at ANY loop count.
// ═══════════════════════════════════════════════════════════════════════════
// per §11.1 ResourceHolder / §22.10 capture rule / §28.4, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           local_resource_holder_base_clause_new_asan_x200_spec_11_1) {
    const std::string src =
        std::string("module L3742c; ") + kRes +
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; "
        "class Derived(a: int): Base(new Res(a)) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(i); "
        "      _ := d.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — EXACTLY-ONCE, base WITHOUT an explicit constructor. This arm already
// evaluated the base-clause arguments exactly once before the fix
// (`prePopulateBasePositionalFields` is the only evaluator when the base call
// routes to the primary); the row pins that the fix did not perturb it.
// MEASURED pre-fix AND post-fix: `bp=1 n=1`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1, issue #3742 (regression control)
BEZEL_TEST(base_clause_new_argument_3742,
           control_side_effecting_arg_evaluated_once_no_explicit_base_ctor) {
    const std::string src =
        "module L3742d; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class Base(bp: int) "
        "  public bx: int := 11; "
        "end Base; "
        "class Derived(a: int): Base(Counter.Next()) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx} n={Counter.N}\"); "
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
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 bp=1 bx=11 n=1") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T5 — EXACTLY-ONCE, base WITH an explicit constructor: the double-evaluation
// pin. `prePopulateBasePositionalFields` and `emitImplicitBase` both walked
// `primaryBaseCtorArgs` and both lowered them, so `Counter.Next()` ran twice.
// FAIL-WITHOUT: `a=7 bp=200 bx=11 n=2` — measured on f517ddb, i.e. BEFORE
// either half of this fix, so this half stands on its own.
// §22.8 step 1 is one call: `n=1`. The explicit constructor multiplies by 100
// and chains to the primary, so `bp=100` also proves WHICH evaluation reached
// the slot.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 "The base or chained constructor call" / §22.10, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           side_effecting_arg_evaluated_once_with_explicit_base_ctor_22_8) {
    const std::string src =
        "module L3742e; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class Base(bp: long) "
        "  public bx: int := 11; "
        "  public constructor(v: int) "
        "    this(v * 100); "
        "  end constructor; "
        "end Base; "
        "class Derived(a: int): Base(Counter.Next()) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx} n={Counter.N}\"); "
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
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("a=7 bp=100 bx=11 n=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=2") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bp=200") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T6 — the EXACTLY-ONCE property with a real ALLOCATION on each evaluation,
// amplified 200x under ASan/LSan. Same base shape as T5 (a base declaring
// BOTH a §22.10 primary constructor and an explicit one that `ctorForArgs`
// resolves for the base-clause argument, so `emitImplicitBase` lowers and
// calls it), but the argument now allocates: a second evaluation is a second
// heap object, which is what turns the T5 defect from a wrong integer into a
// resource fault.
// FAIL-WITHOUT (the codegen half alone, measured on f517ddb): `made=400`.
// Kept SEPARATE from T5 because T5's oracle is the counter alone and can run
// on the plain runner, while this one needs the sanitizer net.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 "The base or chained constructor call" / §28.4, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           allocating_base_clause_arg_evaluated_once_asan_x200) {
    const std::string src =
        std::string("module L3742f; ") + kRes +
        "class Maker "
        "  public static method NextTag(v: int): int "
        "    using var probe := new Res(v); "
        "    return probe.Id() + 1; "
        "  end NextTag; "
        "end Maker; "
        "class Base(bp: long) "
        "  public bx: int := 11; "
        "  public constructor(v: int) "
        "    this(v * 100); "
        "  end constructor; "
        "end Base; "
        "class Derived(a: int): Base(Maker.NextTag(a)) "
        "  public method Get(): long "
        "    return bp; "
        "  end Get; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(3); "
        "      _ := d.Get(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=400") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0 ") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T7 — THE REFUTATION CONTROL. The issue attributed the defect to the
// argument's class TYPE. A base-clause argument of the SAME class type
// produced by a METHOD CALL carries no `ast::Type` node and was ALWAYS
// correct — measured `use=5 made=1` on f517ddb, pre-fix. Keeping this row
// beside T1 is what pins the real discriminator (a nested type node), so a
// future reader does not re-derive the wrong one.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 / §28.1 owned return, issue #3742 (refutation control)
BEZEL_TEST(base_clause_new_argument_3742,
           control_owned_returning_call_argument_worked_before_and_after) {
    const std::string src =
        std::string("module L3742g; ") + kRes + kBaseOwnedBody +
        "class Fact "
        "  public static method Make(v: int): owned Res "
        "    return new Res(v); "
        "  end Make; "
        "end Fact; "
        "class Derived(a: int): Base(Fact.Make(a)) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(5); "
        "      _ := d.Use(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T8 — a `new` NESTED inside a call argument in the base clause. The type
// node is one level deeper than T1's, which is the case `resolveTypesInExpr`'s
// recursion must reach (the `NewExpr` arm recurses into `ne->args`, and the
// `CallExpr` arm into its arguments) rather than only the top-level node.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 / §22.8 step 1, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           nested_new_inside_call_argument_in_base_clause) {
    const std::string src =
        std::string("module L3742h; ") + kRes + kBaseOwnedBody +
        "class Fact "
        "  public static method Wrap(owned p: Res): owned Res "
        "    return p; "
        "  end Wrap; "
        "end Fact; "
        "class Derived(a: int): Base(Fact.Wrap(new Res(a))) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(9); "
        "      _ := d.Use(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("made=200 live=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T9 — the VALUE-shaped argument kinds, all of which worked before and must
// keep working: an arithmetic expression, a string literal and an
// interpolated string. Three bases in one program so a regression in any one
// of them is a single failing row rather than three silently-skipped ones.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 / §22.8 step 1, issue #3742 (regression control)
BEZEL_TEST(base_clause_new_argument_3742,
           control_value_shaped_base_clause_arguments_unchanged) {
    const std::string src =
        "module L3742i; "
        "class BaseI(bp: int) "
        "  public method Get(): int "
        "    return bp; "
        "  end Get; "
        "end BaseI; "
        "class BaseS(s: string) "
        "  public method Get(): string "
        "    return s; "
        "  end Get; "
        "end BaseS; "
        "class DArith(a: int): BaseI(a + 1) end DArith; "
        "class DLit(a: int): BaseS(\"hello\") end DLit; "
        "class DInterp(a: int): BaseS($\"v={a}\") end DInterp; "
        "class DIdent(a: int): BaseI(a) end DIdent; "
        "class C "
        "  public static method Main(): int "
        "    using var w := new DArith(9); "
        "    using var x := new DLit(9); "
        "    using var y := new DInterp(9); "
        "    using var z := new DIdent(9); "
        "    Console.WriteLine($\"w={w.Get()} x={x.Get()} y={y.Get()} z={z.Get()}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("w=10 x=hello y=v=9 z=9") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T10 — IMPORTED base, LOCAL derived, `new` of an IMPORTED class written in
// the inheritance clause. §2.8.3 rule 4: the importer must observe what a
// single-module compile of the same declarations produces (T1). The base
// clause is a property of the DERIVED declaration, so it is resolved in the
// IMPORTER's own compile — which is exactly the pass the fix repairs.
// ═══════════════════════════════════════════════════════════════════════════
// per §2.8.3 rule 4 / §22.10, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           imported_base_new_expression_argument_spec_2_8_3) {
    const std::string lib =
        std::string("module XL3742j; ") +
        kResPublic +
        "public class Base(owned r: Res) "
        "  public method Use(): int "
        "    return r.Id(); "
        "  end Use; "
        "end Base; ";
    const std::string app =
        "module XM3742j; "
        "import XL3742j; "
        "class Derived(a: int): Base(new Res(a)) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(5); "
        "    Console.WriteLine($\"use={d.Use()} made={Res._made}\"); "
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
    BEZEL_EXPECT_TRUE(out.find("use=5 made=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("made=2") == std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T11 — the imported twin of T3 (ResourceHolder), amplified. The dependency
// owns the `owned`-field initializer that consumes the parameter, so this row
// exercises the cross-module disposal path for a value the IMPORTER minted in
// the base clause. Run through the plain runner + explicit counters (the
// hand-rolled two-module runner has no `runAotChecked` counterpart), so
// `made=200 live=0` is the disposal oracle here.
// ═══════════════════════════════════════════════════════════════════════════
// per §2.8.3 rule 4 / §11.1 ResourceHolder / §28.4, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           imported_resource_holder_base_clause_new_x200) {
    const std::string lib =
        std::string("module XL3742k; ") +
        kResPublic +
        "public class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; ";
    const std::string app =
        "module XM3742k; "
        "import XL3742k; "
        "class Derived(a: int): Base(new Res(a)) "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(i); "
        "      _ := d.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live}\"); "
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
    BEZEL_EXPECT_TRUE(out.find("made=400") == std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("made=0") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T12 — THE SIBLING HEADER POSITION: a §22.5 default on a §22.10
// primary-constructor parameter. `Resolver::resolveTypeDecl` resolved the
// parameter's declared TYPE but never its `:= Expr` default, so a `new` there
// hit the identical unresolved-type-node path and the identical wild pointer.
// FAIL-WITHOUT: SIGSEGV — measured on f517ddb.
// The two header expression positions land together because they are one
// defect: neither is reachable by `resolveMember`, which is where every other
// expression in a type declaration gets its nested type nodes resolved.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 "Optional parameters and named arguments" / §22.10, issue #3742
BEZEL_TEST(base_clause_new_argument_3742,
           primary_ctor_parameter_default_new_expression_spec_22_5) {
    const std::string src =
        std::string("module L3742l; ") + kRes +
        "class Holder(a: int, owned r: Res := new Res(77)) "
        "  public method Use(): int "
        "    return r.Id() + a; "
        "  end Use; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var h := new Holder(5); "
        "      _ := h.Use(); "
        "      i := i + 1; "
        "    end while; "
        "    using var h2 := new Holder(5); "
        "    Console.WriteLine($\"use={h2.Use()} made={Res._made}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("use=82 made=201") != std::string::npos);
}


// #5132 pin (spec §9.3 / §11.3 / §22.6, v0.313): the shape this file's rows
// used before #5132 -- a body constructor whose parameter list equals the
// primary's -- is now rejected, which is why the accept rows above use a
// distinct parameter list. Asserts the specific diagnostic, not just failure.
BEZEL_TEST(base_clause_new_argument_3742,
           old_same_param_list_shape_rejected_as_primary_redeclaration_5132) {
    auto r = runAot(
        "module M; "
        "class Base(bp: int) "
        "  public constructor(v: int) "
        "    this(v * 100); "
        "  end constructor; "
        "end Base; "
        "class Derived(a: int): Base(a) "
        "end Derived; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "body constructor redeclares the primary constructor"));
}

}  // namespace bezel::codegen
