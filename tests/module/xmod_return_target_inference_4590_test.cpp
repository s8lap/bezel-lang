// Spec-derived regression test for issue #4590 — §14.5 phase 3 ("Target-typed
// completion") is silently absent across a module boundary.
//
// spec §14.5 "Type-argument inference for generic methods" / §14.1 / §A.7 /
// §2.8.3 rule 2 (imported artifacts must not alter an instantiation's
// semantics) / §25 "Operator overloading", v0.306.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.5 phase 3: "A type parameter still unbound is bound from the call's
// target type, where the call site has one, by unifying the method's declared
// return type … against the target. … A type parameter that remains unbound —
// it appears in no self-determined argument position and no target context
// supplies it — is a compile error naming the parameter: supply explicit type
// arguments, or give the call a target."
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// `ExprTC::inferReturnTargetTypeArgs` implements that phase, and every line of
// it was reached through `md`, the AST-side `ast::MethodDecl*`. `ModuleLoader`
// never assigns `Symbol::decl` for an imported method — the importing
// compilation has no AST for a dependency at all — so `md` is null for EVERY
// imported generic method and the routine `return false`d before doing
// anything, with `*outNeedsInfer` never even set. That is family M-of-#3152
// ("AST-only guard, no imported-side branch"): not "inference declined", but
// the rule's whole enforcement absent on one side of a boundary the spec says
// makes no difference.
//
// ── MEASURED, on cafdfc52 (= origin/main), with real two-module builds ─────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
// The issue filed this unverified ("not yet reproduced with a real two-module
// build") and reported TWO consequences. Both were re-measured; the report is
// half right, and the halves are not the ones it names.
//
//   * UNDER-REJECTION — CONFIRMED, and it is this routine's to fix. The
//     imported `Create4590<T>(): owned T` called with no target COMPILED
//     CLEAN (zero diagnostics at any severity) and RAN, exit 9, where the
//     byte-identical single-module program is rejected. That is T1/T2 below.
//
//   * "SILENT WRONG-TYPE SUBSTITUTION" — the shape reproduces (the same call
//     WITH a target returns 0 cross-module where the local twin returns 1),
//     but it is NOT this routine's defect and this fix does not repair it:
//     the exporter compiles a generic method's body once against the i32
//     placeholder, and for a `new T()` body that body is #2477's "dead
//     abstract template" which #2491 degrades to a null return. Sema now
//     substitutes the call's type correctly and the value is still 0, because
//     the importer never monomorphizes an imported generic method's body
//     (methodBodyText is exported only for generic TYPES — see
//     ModuleMetadataBuilder's #3645 scope-narrow). Under generated-code ASan
//     it is worse than a wrong value: a `stack-buffer-overflow`, READ of size
//     8 at a 4-byte stack object, measured identically on cafdfc52 and with
//     this fix. That residue is the #1937/#428 family and is filed as #4600;
//     no row here asserts anything that would encode it.
//
// A THIRD consequence the issue did not predict fell out of the same repair
// and is pinned here as T3/T4: `unifyReturnAgainstTarget` is shared with
// §25's operator-overload resolution (#1270 — "a type-parameter name bound to
// two conflicting concrete types fails, so the operator does not apply"). Its
// bare-type-parameter recognizer had the same AST-only shape, so that check
// was dead for every imported generic operator: `Box4590<int> + Box4590<string>`
// COMPILED AND RAN cross-module (exit 5) while the identical single-module
// program is rejected. Teaching the recognizer both spellings makes the two
// agree.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ THE BUG. Imported return-only `<T>`, no target -> the SPECIFIC
//        §14.1/§A.7 diagnostic. Pre-fix: compiles clean and runs.
//   T2   DIFFERENTIAL CONTROL. The byte-identical declarations in ONE module,
//        rejected with the SAME text before AND after — so the module
//        boundary is proved to be the whole difference.
//   T3 ★ THE OPERATOR HALF. Imported generic operator at MISMATCHED
//        instantiations -> the SPECIFIC §25 diagnostic. Pre-fix: ran, exit 5.
//   T4   DIFFERENTIAL CONTROL for T3, same twin argument.
//   T5   ANTI-FALSE-REJECT. `Echo4590<T>(x: T): T` — T bound by an ARGUMENT
//        (§14.5 phase 1) WITH a target present: the new imported branch must
//        not hijack it. Runs, exact value 42.
//   T6   ANTI-FALSE-REJECT. A return type mentioning no type parameter at
//        all. Runs, exact value 5.
//   T7   ANTI-FALSE-REJECT. EXPLICIT type arguments and NO target — #747's
//        short-circuit must still apply across the boundary. Runs, exact
//        value 9, and the inference diagnostic must be ABSENT.
//   T8   ANTI-FALSE-REJECT. The same call WITH a target is ADMITTED and the
//        diagnostic is ABSENT — a compile-time claim, with NO run leg, because
//        that program's RUN is #4600's stack-buffer-overflow on both sides of
//        this fix. See the row for the measurement.
//   T9   ANTI-FALSE-REJECT. The imported generic operator at MATCHING
//        instantiations still resolves. Runs, exact value 5.
//
// Every reject row matches a SPECIFIC diagnostic substring rather than
// `failedAtCompileTimeWithRealLocation`, which answers true on both sides of
// any crash-shaped failure and would be vacuous here.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4590 {
    std::string path;
    explicit TempFile4590(std::string p) : path(std::move(p)) {}
    ~TempFile4590() { std::remove(path.c_str()); }
};

void writeSource4590(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled4590 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_imported_symbol_residues_3147_test.cpp established).
template <typename Fn>
std::string captureStderr4590(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz4590_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// Build `libSrc` to an object, then compile `appSrc` against it.
Compiled4590 compileImport4590(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4590_");
    TempFile4590 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4590 aObj((dir / (stem + "_a.o")).string());
    writeSource4590(aSrc.path, libSrc);
    Compiled4590 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile4590 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4590 bExe((dir / stem).string());
    writeSource4590(bSrc.path, appSrc);
    r.err = captureStderr4590([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

// The one-module twin of the above — same capture, no dependency.
Compiled4590 compileLocal4590(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4590l_");
    TempFile4590 aSrc((dir / (stem + ".bzl")).string());
    TempFile4590 aExe((dir / stem).string());
    writeSource4590(aSrc.path, src);
    Compiled4590 r;
    r.err = captureStderr4590([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
//
// `compileFileForTest` / `runProgramForTest` (#4532) are what wire this
// hand-rolled runner into `gate.sh --full`: with
// BEZEL_AOT_FORCE_SANITIZE=address the importer's code is built under
// generated-code ASan and run with the sweep's own options, so a redzone error
// fails the row loudly while the row keeps asserting the VALUE it exists to
// assert.
int buildImportAndRun4590(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4590r_");
    TempFile4590 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4590 libObjF((dir / (id + "_lib.o")).string());
    TempFile4590 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile4590 mainExeF((dir / id).string());
    writeSource4590(libSrcF.path, libSrc);
    writeSource4590(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. One module carrying every shape the rows exercise.
// ---------------------------------------------------------------------------
//
//   U4590.Create4590<T>(): owned T   §14.5 phase 3 — T is RETURN-ONLY   (T1,T7,T8)
//   U4590.Echo4590<T>(x: T): T       §14.5 phase 1 — T bound by an arg  (T5)
//   U4590.Count4590<T>(x: T): int    no type parameter in the return    (T6)
//   Box4590<T> + Box4590<T>          §25/#1270 — the shared unifier     (T3,T9)
const char* kDeclsBody4590 =
    "public class Widget4590 "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Widget4590; "
    "public static class U4590 "
    "  public static method Create4590<T>(): owned T where T: class, new() "
    "    return new T(); "
    "  end Create4590; "
    "  public static method Echo4590<T>(x: T): T "
    "    return x; "
    "  end Echo4590; "
    "  public static method Count4590<T>(x: T): int "
    "    return 1; "
    "  end Count4590; "
    "end U4590; "
    "public class Box4590<T> "
    "  public readonly property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public static method operator +(a: Box4590<T>, b: Box4590<T>): "
    "      owned Box4590<T> "
    "    return new Box4590<T>(a.V + b.V); "
    "  end operator +; "
    "end Box4590; ";

std::string dep4590() {
    return std::string("module Dep4590; ") + kDeclsBody4590;
}

// The importing module, parameterised by the body of `Main`.
std::string app4590(const std::string& tag, const std::string& mainBody) {
    return "module App4590" + tag + "; import Dep4590; " +
           "class C4590 public static method Main(): int " + mainBody +
           " end Main; end C4590;";
}

// The SINGLE-MODULE twin: the same declarations and the same `Main` body,
// with no module boundary anywhere.
std::string local4590(const std::string& tag, const std::string& mainBody) {
    return "module Loc4590" + tag + "; " + kDeclsBody4590 +
           "class C4590 public static method Main(): int " + mainBody +
           " end Main; end C4590;";
}

bool mentions4590(const Compiled4590& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// §14.1/§A.7, as `reportNoTargetInferenceIfNeeded` emits it.
const char* kNoTargetHead4590 =
    "cannot infer the type argument of `Create4590`'s call";
const char* kNoTargetTail4590 = "a target type is required";
// §25, as the operator-resolution path emits it.
const char* kOperatorReject4590 =
    "operator `+` is not defined for type `Box4590` with these operands";

// The `Main` bodies, shared between each cross-module row and its twin.
const char* kNoTargetBody4590 =
    "using var w := U4590.Create4590(); return 9;";
const char* kOperatorMixBody4590 =
    "using var a := new Box4590<int>(2); "
    "using var b := new Box4590<string>(3); "
    "using var c := a + b; var n := c.V; return n;";

}  // namespace

// ===========================================================================
// T1 — ★ THE BUG. An imported generic method whose type parameter appears only
// in its return type, called with NO target. §14.5 phase 3 says this is "a
// compile error naming the parameter"; across a module boundary the diagnostic
// never fired, so the program COMPILED CLEAN and RAN (measured on cafdfc52:
// exit 9, zero diagnostics at any severity).
// // per §14.5 phase 3 / §14.1 / §A.7 / §2.8.3 rule 2, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           imported_return_only_type_param_without_target_is_rejected) {
    auto r = compileImport4590(dep4590(), app4590("T1", kNoTargetBody4590));
    if (!r.depOk) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4590(r, kNoTargetHead4590));
    BEZEL_EXPECT_TRUE(mentions4590(r, kNoTargetTail4590));
}

// ===========================================================================
// T2 — DIFFERENTIAL CONTROL for T1. The byte-identical declarations and call
// in ONE module. This was already rejected, before AND after, so the module
// boundary is proved to be the whole difference T1 measures.
// // per §14.5 phase 3 / §2.8.3 rule 2, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           same_module_twin_rejects_identically) {
    auto r = compileLocal4590(local4590("T2", kNoTargetBody4590));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4590(r, kNoTargetHead4590));
    BEZEL_EXPECT_TRUE(mentions4590(r, kNoTargetTail4590));
}

// ===========================================================================
// T3 — ★ THE OPERATOR HALF of the same recognizer. `unifyReturnAgainstTarget`
// is shared with §25 operator resolution (#1270: an operator on a generic type
// applies only when both operands unify ONE binding for its type parameter).
// Its bare-type-parameter test was AST-only too, so for an IMPORTED generic
// class the check could never bind and therefore never conflict:
// `Box4590<int> + Box4590<string>` compiled and RAN (measured on cafdfc52:
// exit 5) while the single-module twin (T4) is rejected.
// // per §25 "Operator overloading" / §14.1 / §2.8.3 rule 2, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           imported_generic_operator_rejects_mismatched_instantiations) {
    auto r = compileImport4590(dep4590(), app4590("T3", kOperatorMixBody4590));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4590(r, kOperatorReject4590));
}

// ===========================================================================
// T4 — DIFFERENTIAL CONTROL for T3, same argument as T2.
// // per §25 / §2.8.3 rule 2, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           same_module_operator_twin_rejects_identically) {
    auto r = compileLocal4590(local4590("T4", kOperatorMixBody4590));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4590(r, kOperatorReject4590));
}

// ===========================================================================
// T5 — ANTI-FALSE-REJECT. §14.5 phase 1: `Echo4590<T>(x: T): T` binds T from
// the ARGUMENT, so phase 3 does not apply and no target is required — even
// though a target is present here. The imported branch must read the
// reconstructed formal types (`formalParamResolvedType`) and reach the same
// "already bound" answer the AST walk reaches locally; reading no formals at
// all would make every imported parameter look unbound and turn this into a
// false reject. Asserts the RUN's exact value.
// // per §14.5 phase 1, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           argument_bound_type_param_still_accepted_cross_module) {
    int rc = buildImportAndRun4590(
        dep4590(),
        app4590("T5", "var x: int := U4590.Echo4590(41); return x + 1;"));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T6 — ANTI-FALSE-REJECT. A generic method whose declared return type mentions
// no type parameter at all needs no target under any phase.
// // per §14.5 phase 3, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           non_parametric_return_needs_no_target_cross_module) {
    int rc = buildImportAndRun4590(
        dep4590(),
        app4590("T6", "var n := U4590.Count4590(5); return n + 4;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T7 — ANTI-FALSE-REJECT. §14.5 "Explicit lists are all-or-nothing": when a
// full explicit type-argument list is supplied, "no inference runs" — so no
// target is required even for a return-only parameter (#747 R43). That
// short-circuit compares the list against the method's type-parameter COUNT,
// which the imported branch must take from `MethodSymbol::genericParams`;
// getting the count wrong here is a false reject of a fully-explicit call.
// // per §14.5 "Explicit lists are all-or-nothing", v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           explicit_type_arguments_need_no_target_cross_module) {
    auto c = compileImport4590(
        dep4590(),
        app4590("T7", "using var w := U4590.Create4590<Widget4590>(); "
                      "return 9;"));
    if (!c.depOk) return;
    BEZEL_EXPECT_TRUE(c.success);
    BEZEL_EXPECT_FALSE(mentions4590(c, kNoTargetHead4590));

    int rc = buildImportAndRun4590(
        dep4590(),
        app4590("T7r", "using var w := U4590.Create4590<Widget4590>(); "
                       "return 9;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 9);
}

// ===========================================================================
// T8 — ANTI-FALSE-REJECT. The same return-only call WITH a target: §14.5 phase
// 3 solves T from the typed local, so the call must be ADMITTED and the T1
// diagnostic must be ABSENT. That is the whole claim of this row, and it is a
// COMPILE-TIME claim.
//
// ⚠ DELIBERATELY NO RUN LEG, and this is not a weakened oracle — it is the
// opposite. The program does compile, but it MISCOMPILES for a reason that is
// #4600's and not this fix's: the importer lowers the imported call's return
// through `importedReturnType`'s still-unsubstituted bare `T`, i.e. the i32
// placeholder, while the local slot is pointer-sized. Under generated-code
// ASan (`BEZEL_AOT_FORCE_SANITIZE=address`) the run is a
// `stack-buffer-overflow`, READ of size 8 at a 4-byte stack object in `main`
// — MEASURED IDENTICALLY on cafdfc52 and with this fix applied, so it is
// pre-existing and untouched here. A run leg would therefore be red on BOTH
// sides of this fix: it would pin #4600, not #4590, and it would red the
// gate's `--full` leg for a defect this commit does not claim to fix. T7's run
// leg (explicit type arguments) is the ASan-clean twin — measured clean on
// both sides — and is what covers the runtime side of the imported
// return-only shape here.
// // per §14.5 phase 3, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           target_typed_local_admits_the_call_cross_module) {
    auto c = compileImport4590(
        dep4590(),
        app4590("T8", "using var w: Widget4590 := U4590.Create4590(); "
                      "return 9;"));
    if (!c.depOk) return;
    BEZEL_EXPECT_TRUE(c.success);
    BEZEL_EXPECT_FALSE(mentions4590(c, kNoTargetHead4590));
}

// ===========================================================================
// T9 — ANTI-FALSE-REJECT for T3. The imported generic operator at MATCHING
// instantiations must still resolve and compute: teaching the unifier the
// imported spelling of a bare type parameter makes conflicting bindings fail,
// and must leave an agreeing binding alone. Asserts the RUN's exact value.
// // per §25 / §14.1, v0.306
// ===========================================================================
BEZEL_TEST(xmod_return_target_inference_4590,
           imported_generic_operator_accepts_matching_instantiations) {
    int rc = buildImportAndRun4590(
        dep4590(),
        app4590("T9", "using var a := new Box4590<int>(2); "
                      "using var b := new Box4590<int>(3); "
                      "using var c := a + b; var n := c.V; return n;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

}  // namespace bezel::module
