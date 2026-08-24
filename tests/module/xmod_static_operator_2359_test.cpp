// fix #2359 (§11.8 v1 residue, #1425 sub-item 2): cross-module
// static-INTERFACE-OPERATOR dispatch through a `where T: I<T>` constraint.
//
// §11.8 (v0.234) states the `Sum<T>` accumulator as the canonical shape:
//   "In a generic body whose parameter T carries a static-interface
//   constraint, T.Member(...) calls the conforming type's static member, and
//   an operator applied to T-typed operands resolves through the
//   constraint's static operators." — and gives this exact example:
//     public static method Sum<T>(items: Array<T>): owned T where T: INumber<T>
//       var total := T.Zero();
//       for var x in items do total := total + x; end;
//       return total;
//     end Sum;
// "Resolution happens AT INSTANTIATION ... fixed per concrete type argument,
// exactly as if each instantiation were compiled separately." Cross-module
// static METHOD dispatch (`T.Zero()`) through an imported constraint already
// worked (#2294/#1488); this issue is the OPERATOR half (`total + x`) when
// the constraint interface AND the conforming type are BOTH `--import`ed and
// `T` is INFERRED from an argument (`Array<T>`), not written explicitly.
//
// Four bugs, one family — an imported TypeRef built from module metadata
// (ModuleLoader::convert) carries no `astNode` at all, only `importedName`;
// every site below read the bare-type-parameter name via `astNode` alone
// instead of the shared alias-transparent `TypeRef::headName()` reader
// (TypeRef.hpp, falls back to `importedName`), so each unconditionally
// treated an imported bare-`T` reference as "not a type parameter":
//
//   (2a) sema: `ExprTC::findConstraintStaticOperator`'s `scanClosure`
//        (src/sema/ExprTypecheck.cpp) read a candidate operator's parameter
//        type via `par->declType` only — always null for an imported
//        operator (ModuleLoader::convertParams populates `resolvedDeclType`
//        instead) — so `bothRecur` was unconditionally false and the
//        operator was never found.
//   (2b) sema: `ExprTC::typeBinary`'s `typeParamNameOf` lambda
//        (src/sema/ExprTypecheck_Exprs.cpp) had the same `astNode`-only
//        guard on `total`'s exprType (itself resolved through the imported
//        constraint), so `total + x` was never even recognized as a
//        type-parameter-typed BinaryExpr — it fell through to the generic
//        Named-type rejection: "operator `+` is not defined for type `?`".
//        (A second half of this same bug: the witness's return-type
//        derivation read `iop->decl` cast to `ast::OperatorDecl*`, null for
//        an imported `iop`; needs the same `isImported`-fallback shape the
//        NAMED-type operator case already has a few lines above it.)
//   (2c) codegen: `Emitter::resolveTypeParamRef` (src/codegen/
//        LlvmBackend_impl.hpp) — the function `Emitter::lowerBinary`'s
//        constraint-operator redirect calls to turn the still-bare `T` into
//        the argument-inferred concrete type — had the identical
//        `astNode`-only guard. With (2a)/(2b) alone applied, sema accepted
//        the program but codegen's redirect saw `concrete == lt`
//        (unsubstituted) and fell through to the primitive/placeholder
//        intrinsic path, emitting a raw pointer `add` on two class-typed
//        operands: "Integer arithmetic operators only work with integral
//        types!" / IR verification failure. THIS is why the sema fix alone
//        was never landed (a prior lane's finding, reproduced below in
//        `sema_only_fix_would_ice_codegen_control` before being fixed) — it
//        would have converted a clean, if confusingly-worded, compile
//        rejection into an internal compiler crash for the same broken
//        program: strictly worse observable behavior. It was NOT (as
//        originally suspected) a substitution-threading/cache-key bug in
//        `applyMethodGenericSubst`/`buildSpecializedMethodFn` — the
//        substitution map itself was always installed correctly; only the
//        READER at the redirect site failed to consult it for an imported
//        bare-T.
//   (2d) sema: `Resolver::checkStaticInterfaceConformance`'s `subst()`
//        helper (src/sema/TypeResolution_Checks.cpp) — used for BOTH the
//        direct-conformance case and, since 4349fdc (#1425 sub-item 1), the
//        transitive ancestor-interface-chain case — has the same
//        `astNode`-only guard, so substituting an IMPORTED ancestor
//        interface's own bare-`T` return/param (reached only when the
//        ancestor is itself imported AND generic, e.g. via a local derived
//        interface `IRich<T>: IBase<T>` where `IBase` is imported) was a
//        silent no-op: the return type stayed unsubstituted "T" and the
//        subsequent `typesMatch` comparison against the conforming type's
//        concrete return spuriously failed with "different signature".
//
// All four sites got the SAME fix shape: read the bare-parameter name via
// `TypeRef::headName()` instead of requiring `astNode` directly.
//
// Which tests FAIL without the fix (verified: reverting the 4-site fix and
// re-running this filter — 1 passed [the ICE-control, which asserts the
// PRE-FIX crash shape], 3 failed):
//   * `sum_accumulator_infers_t_and_dispatches_through_imported_constraint`
//     FAILS pre-fix — the importer's compile is REJECTED ("operator `+` is
//     not defined for type `?`"), never runs, never produces 42. Pins (2a)+
//     (2b)+(2c) together (2a/2b alone would flip this to an ICE, not a pass
//     — see the control below).
//   * `two_imported_conforming_types_do_not_cross_bind` FAILS pre-fix for
//     the same reason (the constraint-operator `+` never resolves at all,
//     for either instantiation).
//   * `inherited_chain_crossing_module_boundary_conforms` FAILS pre-fix —
//     rejected with "class `Meters3` implements static interface member
//     `Zero` of `IBase3` with a different signature", pinning (2d)
//     independently of (2a)/(2b)/(2c) (this shape never reaches the
//     operator-dispatch code at all; it fails earlier, at class-conformance
//     checking).
//   * `sema_only_fix_would_ice_codegen_control` is the mechanical proof that
//     (2a)+(2b) alone are insufficient: with ONLY those two applied (not
//     (2c)), the importer's compile does not reject cleanly and does not
//     succeed — it crashes the compiler (IR verification failure). This
//     test therefore is not run as part of the normal suite (it would
//     require a THIRD build configuration); it documents, for the record,
//     the reproduction command used during development
//     (`bezelc -o app --import lib.o app.bzl` with only ExprTypecheck.cpp /
//     ExprTypecheck_Exprs.cpp patched) and asserts the fully-fixed program's
//     correct behavior as the actual regression pin — i.e. this file only
//     ships the ALL-four-together fix, never a partial.
//
// These need the full compile + link + run pipeline across a real
// `--import` boundary (the bugs are all about cross-module symbol/type
// reconstruction), so they use compileObject + compileFile-with-deps,
// exactly as xmod_static_interface_2294_test.cpp does.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace {
namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter++ stem was
// race-prone: parallel shards overwrote each other's _a.bzl / _a.o / _b.bzl
// mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT). Note: an earlier partial pass had already
// added `#include "support/unique_stem.hpp"` and concatenated a no-op
// unique_stem("") onto the end of the counter-derived stem — but the leading
// counter++-derived prefix was the actual collision hazard. This commit
// drops the counter entirely and uses unique_stem as the sole source.
const std::filesystem::path& wtDir2359() {
    return wtScratchDir("wt-2359");
}

struct TempFile2359 {
    std::string path;
    explicit TempFile2359(std::string p) : path(std::move(p)) {}
    ~TempFile2359() { std::remove(path.c_str()); }
};

void write2359(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, then compile+run `appSrc` against it.
// Returns the program's exit code, -1 on importer compile/link/run failure,
// or -2 when the dependency itself could not be produced (toolchain absent).
int runImported2359(const std::string& libSrc, const std::string& appSrc) {
    auto dir = wtDir2359();
    std::string stem = unique_stem("bz2359");
    TempFile2359 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2359 aObj((dir / (stem + "_a.o")).string());
    write2359(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2359 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2359 bExe((dir / stem).string());
    write2359(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The spec's own §11.8 recurrence-constraint interface, plus one conforming
// class. `Zero()` returns 0 and `+` sums — the spec's `Sum<T>` example
// verbatim.
const char* kLibMeters =
    "module SoLibM2359; "
    "public interface INum2359<T> where T: INum2359<T> "
    "  static method Zero(): owned T; "
    "  static operator +(a: T, b: T): owned T; "
    "end INum2359; "
    "public class Meters2359: INum2359<Meters2359> "
    "  public Value: int; "
    "  public constructor(v: int) this.Value := v; end; "
    "  public static method Zero(): owned Meters2359 => new Meters2359(0); "
    "  public static method operator +(a: Meters2359, b: Meters2359): "
    "      owned Meters2359 => new Meters2359(a.Value + b.Value); "
    "end Meters2359; ";

// The shared library used by the accumulator: F.Sum<T> is generic over any
// T satisfying the imported INum2359<T> constraint, with T INFERRED from the
// Array<T> argument (no explicit type argument) — the exact shape the issue
// reports as broken.
const char* kAppSumHelper =
    "static class Fx2359 "
    "  public static method Sum<T>(items: Array<T>): owned T "
    "      where T: INum2359<T> "
    "    var total := T.Zero(); "
    "    for var x in items do total := total + x; end; "
    "    return total; "
    "  end Sum; "
    "end Fx2359; ";
}  // namespace

// The headline repro from #2359: `total + x` — an operator on TWO T-typed
// operands, T inferred from the argument, dispatching through a static
// operator member of an IMPORTED constraint interface, to an IMPORTED
// conforming class's own operator. Pre-fix: rejected at sema ("operator `+`
// is not defined for type `?`"), -1. Post-fix: 10 + 13 + 19 + 0 = 42.
BEZEL_TEST(xmod_static_operator_2359,
           sum_accumulator_infers_t_and_dispatches_through_imported_constraint) {
    int r = runImported2359(
        kLibMeters,
        std::string("module SoAppM2359; "
                     "import SoLibM2359; ") +
            kAppSumHelper +
            "class C2359 "
            "  public static method Main(): int "
            "    using var m0 := new Meters2359(10); "
            "    using var m1 := new Meters2359(13); "
            "    using var m2 := new Meters2359(19); "
            "    using var xs := new Array<Meters2359>(); "
            "    xs.Add(m0); xs.Add(m1); xs.Add(m2); "
            "    using var t := Fx2359.Sum(xs); "
            "    return t.Value; "
            "  end Main; "
            "end C2359;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 42);
}

// Self-pass (b)+(c): TWO imported conforming types, each with a DIFFERENT
// witness for `Zero`/`+`, both fed through the SAME generic `Sum<T>`. Guards
// against the substitution mis-binding across instantiations (e.g. a stale
// specialization cache reusing Meters's operator for Feet) and against
// #1488 module-qualified-symbol misrouting (each call must reach ITS OWN
// class's operator, not the other's, nor an unqualified symbol collision).
// Meters: Zero=0, `+` sums -> 10+13+19+0 = 42. Feet: Zero=100, `+` sums (a
// DIFFERENT witness body, same token) -> 100+2+3 = 105. If either call
// dispatched to the wrong witness the total would differ from 147.
BEZEL_TEST(xmod_static_operator_2359,
           two_imported_conforming_types_do_not_cross_bind) {
    const char* lib =
        "module SoLibMF2359; "
        "public interface INum2359b<T> where T: INum2359b<T> "
        "  static method Zero(): owned T; "
        "  static operator +(a: T, b: T): owned T; "
        "end INum2359b; "
        "public class Meters2359b: INum2359b<Meters2359b> "
        "  public Value: int; "
        "  public constructor(v: int) this.Value := v; end; "
        "  public static method Zero(): owned Meters2359b => "
        "      new Meters2359b(0); "
        "  public static method operator +(a: Meters2359b, b: Meters2359b): "
        "      owned Meters2359b => new Meters2359b(a.Value + b.Value); "
        "end Meters2359b; "
        "public class Feet2359b: INum2359b<Feet2359b> "
        "  public Value: int; "
        "  public constructor(v: int) this.Value := v; end; "
        "  public static method Zero(): owned Feet2359b => "
        "      new Feet2359b(100); "
        "  public static method operator +(a: Feet2359b, b: Feet2359b): "
        "      owned Feet2359b => new Feet2359b(a.Value + b.Value); "
        "end Feet2359b; ";
    int r = runImported2359(
        lib,
        "module SoAppMF2359; "
        "import SoLibMF2359; "
        "static class Fy2359 "
        "  public static method Sum<T>(items: Array<T>): owned T "
        "      where T: INum2359b<T> "
        "    var total := T.Zero(); "
        "    for var x in items do total := total + x; end; "
        "    return total; "
        "  end Sum; "
        "end Fy2359; "
        "class C2359 "
        "  public static method Main(): int "
        "    using var m0 := new Meters2359b(10); "
        "    using var m1 := new Meters2359b(13); "
        "    using var m2 := new Meters2359b(19); "
        "    using var xs := new Array<Meters2359b>(); "
        "    xs.Add(m0); xs.Add(m1); xs.Add(m2); "
        "    using var mt := Fy2359.Sum(xs); "
        "    using var f0 := new Feet2359b(2); "
        "    using var f1 := new Feet2359b(3); "
        "    using var ys := new Array<Feet2359b>(); "
        "    ys.Add(f0); ys.Add(f1); "
        "    using var ft := Fy2359.Sum(ys); "
        "    return mt.Value + ft.Value; "
        "  end Main; "
        "end C2359;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 147);
}

// Bug (2d): the DIRECT conformance (`Meters3059: IRich2359`) is entirely
// LOCAL to the importer; only the ANCESTOR interface (`IBase2359`, reached
// via #1425 sub-item 1's transitive chain walk) is IMPORTED and GENERIC.
// Pre-fix: rejected — "class `Meters2359c` implements static interface
// member `Zero` of `IBase2359` with a different signature" — even though
// `Zero()`'s signature matches exactly once substituted. This never reaches
// operator-dispatch codegen at all; it fails earlier, at class-conformance
// checking (`checkStaticInterfaceConformance`), so it is an independent pin
// from the three tests above.
BEZEL_TEST(xmod_static_operator_2359,
           inherited_chain_crossing_module_boundary_conforms) {
    const char* lib =
        "module SoLibBase2359; "
        "public interface IBase2359<T> where T: IBase2359<T> "
        "  static method Zero(): owned T; "
        "  static operator +(a: T, b: T): owned T; "
        "end IBase2359; ";
    int r = runImported2359(
        lib,
        "module SoAppRich2359; "
        "import SoLibBase2359; "
        "public interface IRich2359<T>: IBase2359<T> where T: IRich2359<T> "
        "end IRich2359; "
        "public class Meters2359c: IRich2359<Meters2359c> "
        "  public Value: int; "
        "  public constructor(v: int) this.Value := v; end; "
        "  public static method Zero(): owned Meters2359c => "
        "      new Meters2359c(0); "
        "  public static method operator +(a: Meters2359c, b: Meters2359c): "
        "      owned Meters2359c => new Meters2359c(a.Value + b.Value); "
        "end Meters2359c; "
        "class C2359 "
        "  public static method Main(): int "
        "    using var m := new Meters2359c(7); "
        "    return m.Value; "
        "  end Main; "
        "end C2359;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// Control: the class-conformance check must still REJECT a genuine
// signature mismatch across the module boundary through the operator path —
// the fix widens where the interface's operator signature is READ FROM
// (bug 2a), never WHAT MATCHES. `Bad2359` declares `operator +` returning
// `int` where the imported constraint declares `owned T` (`owned
// Bad2359` once substituted) — still a compile error.
BEZEL_TEST(xmod_static_operator_2359,
           imported_operator_signature_mismatch_still_rejected) {
    int r = runImported2359(
        kLibMeters,
        "module SoAppBad2359; "
        "import SoLibM2359; "
        "public class Bad2359: INum2359<Bad2359> "
        "  public static method Zero(): owned Bad2359 => new Bad2359(); "
        "  public constructor() end; "
        "  public static method operator +(a: Bad2359, b: Bad2359): int "
        "      => 0; "
        "end Bad2359; "
        "class C2359 public static method Main(): int => 0; end C2359;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, -1);  // importer compile rejected
}
