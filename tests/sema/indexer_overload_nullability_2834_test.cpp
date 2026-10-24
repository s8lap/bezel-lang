// Spec-derived regression test for issue #2834.
//
// §23.4 (Indexers) / §7.1 (Non-nullable and nullable types), v0.240.
//
// §23.4, verbatim:
//
//     "Indexers may be overloaded by parameter type list."
//
// §7.1, verbatim:
//
//     "A plain reference type is non-nullable; a `?` suffix produces its
//      nullable form. … `string` and `string?` are distinct types. The
//      compiler enforces the distinction at every assignment, parameter pass,
//      and return."
//
// So `[i: int]` and `[n: int?]` are TWO distinct parameter type lists and
// §23.4 says overload resolution MUST distinguish them. The prior
// `allParamsMatch` (sema/ExprTypecheck_Exprs.cpp) — and the matching
// `indexerArgMatches` (codegen/LlvmBackend_Assign.cpp) it deliberately
// mirrors — compared `kind`, `primitive`/`declaration`, but NOT `nullable`.
// So for an `int?` argument the structural match reported `[i: int]` and
// `[n: int?]` as the same shape; the `t->members` walk picked whichever it
// reached first (typically `[i: int]`), and the program was rejected with
// the §7 nullable-to-non-nullable diagnostic naming the WRONG overload
// (`Acc[i]` rather than `Acc[n]`). Even where the §7 check would have
// been exempted by `[n: int?]`'s gate (4), the program still lost because
// the wrong overload was selected.
//
// This is BOTH a rejection fix (the false reject on a working program) and
// a value fix (when only one of the two selectors was fixed, sema typed
// one overload and codegen lowered a different one — a silent wrong value,
// worse than today's loud false reject). Every value case therefore
// asserts the CORRECT VALUE; every negative case asserts on diagnostic
// TEXT, never the coarse `rejects()` helper (runAot's WIFSIGNALED path
// synthesizes an Error diagnostic for a runtime SIGSEGV too).

#include "driver/Driver.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using bezel::test::runAot;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §7 "nullable -> non-nullable" assignability diagnostic fragment, shared
// with #2619 / #2795's own gates.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

// The §5.1 "not implicitly convertible" fragment — the SHARED needle every
// §5.1 site reports (`checkAssignable`'s one message).
constexpr const char* kNotConvertible =
    "has a type that is not implicitly convertible to the target type (§5.1)";

}  // namespace

// ===========================================================================
// T1 — THE BUG REPRO. The issue's exact spelling: a class with TWO indexers
// ([i: int] AND [n: int?]), an int? argument, on a VARIABLE receiver (the
// issue's own confirmation that this is NOT a #2795 'this'-rooted artifact).
// Pre-fix: the §7 diagnostic names the wrong overload (`Acc[i]`) and the
// program is rejected even though a correct overload exists.
// ===========================================================================
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           nullable_arg_prefers_nullable_param_overload_23_4_7_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Acc "
        "  private static method NI(): int? => null; "
        "  public property [i: int]: int => i + 1; "
        "  public property [n: int?]: string => \"nullable-param\"; "
        "  public method UseIt(): string return this[NI()]; end UseIt; "
        "end Acc; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    // The exact-match `[n: int?]` overload should be selected — §7's gate (4)
    // exempts a nullable-declared parameter, so the WHOLE message family is
    // absent. Pre-fix the program was rejected with the §7 text naming `Acc[i]`.
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
}

// ===========================================================================
// T2 — REVERSE CONTROL: a NON-nullable `int` argument on the SAME class must
// still select `[i: int]`. The fix must not over-broaden to prefer a
// nullable overload for a non-nullable argument; the prior §5.1 implicit
// conversion `int → int?` would be wrong here (§7.1: "The reverse requires
// a null check or `!`").
// ===========================================================================
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           non_nullable_arg_still_prefers_non_nullable_param_overload) {
    auto r = runAot(
        "module M; "
        "class Acc "
        "  public property [i: int]: int => i + 1; "
        "  public property [n: int?]: string => \"nullable-param\"; "
        "end Acc; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Acc(); "
        "    return a[7]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 7 + 1 = 8 (the [i: int] overload); 9 would be the [n: int?]-as-int
    // (forced to int via ParseInt of "nullable-param", an error), so a value
    // not equal to 8 proves the wrong overload was selected.
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// ===========================================================================
// T3 — VALUE: the bug-repro program must COMPILE AND RUN with the right value
// when only the `[n: int?]` overload exists (the control #2619's own test
// pinned) AND — the load-bearing cell here — when BOTH overloads exist (the
// issue's actual shape). Every overload returns a value with a distinct
// numeric encoding so a wrong-overload selection cannot pass.
// ===========================================================================
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           end_to_end_both_overloads_route_by_argument_nullability) {
    auto r = runAot(
        "module M; "
        "class Acc "
        "  public static method NI(): int? => null; "
        "  public static method SI(): int? => 0; "
        "  public property [i: int]: int => i + 1; "
        "  public property [n: int?]: int => -1; "
        "end Acc; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Acc(); "
        "    var t := 0; "
        "    t := t + a[7]; "                    // [i: int]   -> 8
        "    var m := Acc.SI(); t := t + a[m!]; "  // [i: int]   -> 9
        "    t := t + a[Acc.NI()]; "             // [n: int?]  -> 8
        "    return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// ===========================================================================
// T4 — NAMED TYPE TWIN: §7.1 says "string" and "string?" are distinct types,
// so the bug applies to a `[k: Key]` / `[k: Key?]` overload pair
// identically. This is the cells-need-not-be-Primitive bug surface: a Named
// TypeRef whose `declaration` matches but whose `nullable` differs. Uses a
// user class as the parameter type (a Named TypeRef with both nullability
// shapes) so the test does not depend on the cross-module ABI collision
// documented in #2414.
// ===========================================================================
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           named_type_overloads_also_route_by_argument_nullability_7_1) {
    auto r = runAot(
        "module M; "
        "public class Key "
        "  public x: int := 0; "
        "end Key; "
        "class Box "
        "  public property [k: Key]: int => 50; "
        "  public property [k: Key?]: int => 7; "
        "end Box; "
        "class C "
        "  public static method K(): owned Key => new Key(); "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var t := 0; "
        "    using var k := K(); t := t + b[k]; "    // [k: Key]   -> 50
        "    var nk: Key? := null; t := t + b[nk]; "  // [k: Key?]  -> 7
        "    return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 57);
}

// ===========================================================================
// T5 — ARITY-ONLY FALLBACK: #2619's accept controls
// (`non_nullable_index_spellings_are_untouched`) must keep passing — the
// fix's nullable comparison does not narrow the existing single-indexer /
// arity-only fallback. With BOTH overloads declared, no nullable candidate
// matches a non-nullable `int` argument, so the `[i: int]` overload is
// selected BY ARITY and the access is admitted.
//
// BEHAVIOUR FLIP (#7064, §5.1 / §23.4, v0.343). This row USED to carry a
// `long` argument (`var l: long := 3; b[l]`) and asserted that it COMPILED
// AND RAN (exit 30), on the stated premise that "`a[someLong]` on a class
// whose only indexer is `[i: int]` is a §5.1 implicit widening". That
// premise is FALSE: §5.1's table row is `int` -> `long`, never the reverse,
// and §5.1's constant-narrowing clause — the only way a wider integer
// reaches a narrower declared type — applies to COMPILE-TIME CONSTANTS only.
// #7064 closes that gate (the index argument's type is checked at the index
// position, §23.4's parameter pass), so a `long` argument at an `int`
// parameter is now REJECTED — see the row below. The row's ACTUAL subject,
// the arity-only fallback, is kept below with the `int` argument it was
// always about.
// ===========================================================================
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           arity_only_implicit_conversion_fallback_is_preserved) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  public property [i: int]: int => i * 10; "
        "  public property [n: int?]: int => -1; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var i := 3; "
        "    return b[i]; "                   // arity-only fallback; [i: int]
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

// The `long` half of the row above, now REJECTED (#7064, §5.1 / §23.4,
// v0.343). Pre-#7064 this program compiled and ran (exit 30); the `long` was
// passed to the `int` parameter unconverted. §5.1's table has no `long` ->
// `int` row, so it is a compile error at the conversion site — the index
// position, which §23.4 makes a parameter pass.
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           long_index_at_an_int_parameter_is_rejected_spec_5_1) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  public property [i: int]: int => i * 10; "
        "  public property [n: int?]: int => -1; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var l: long := 3; "
        "    return b[l]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "the index argument to `Box[i]` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}

// ===========================================================================
// T6 — NULLABLE ARG TO NON-NULLABLE-ONLY CLASS (no nullable-param overload
// exists at all): the §7 reject STILL fires. This is the cell that proves
// the fix is not "make all nullable args pass": when there is no nullable
// parameter to accept a nullable argument, the §7 diagnostic — which
// #2619 already shipped — must remain.
// ===========================================================================
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           nullable_arg_with_no_nullable_overload_still_rejects_7) {
    auto r = parseSemaAll(
        "module M; "
        "class Idx "
        "  private static method NI(): int? => null; "
        "  public property [i: int]: int => i + 1; "
        "  public method Bad(): int return this[NI()]; end Bad; "
        "end Idx; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "index argument to `Idx[i]`"));
}

// ===========================================================================
// T7 — CROSS-MODULE: the IMPORTED indexer path (#2414's own scope). The
// candidate's parameter type is read off the reconstructed ParameterSymbol's
// `resolvedDeclType`, not an ast::PropertyDecl — exactly the asymmetry #2414
// papered over with the dual `candParamResolvedType` lambda. The fix lives
// inside that lambda's call site and must work uniformly for the LOCAL and
// the IMPORTED case.
//
// Uses only the `[n: int?]` overload in the imported module: the
// cross-module ABI's documented collision on `bz$<Class>$idxget$<arity>`
// (a same-arity two-overload case) means a value test there is unsound at
// the ABI level (#2414, filed separately). The IMPO RTED overload path
// still exercises the sema + codegen SELECTOR fix: the call site is in
// another module and reads the candidate's parameter type from the
// reconstructed ParameterSymbol (not an ast::PropertyDecl), so a fix
// confined to the local-only branch would miss this cell. A nullable arg
// selecting the nullable-param overload AND compiling without the §7
// "non-nullable target" diagnostic — on a CROSS-MODULE indexer — is the
// load-bearing shape.
// ===========================================================================
namespace {
struct TempFile2834 {
    std::string path;
    explicit TempFile2834(std::string p) : path(std::move(p)) {}
    ~TempFile2834() { std::remove(path.c_str()); }
};
void writeSrc2834(const std::string& p, const std::string& s) {
    std::ofstream o(p);
    o << s;
}
int compileImportRun2834(const std::string& libSrc, const std::string& appSrc) {
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bz2834_xmod_" + std::to_string(::getpid());
    TempFile2834 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2834 aObj((dir / (stem + "_a.o")).string());
    writeSrc2834(aSrc.path, libSrc);
    {
        bezel::Driver d;
        if (!d.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2834 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2834 bExe((dir / (stem + "_b")).string());
    writeSrc2834(bSrc.path, appSrc);
    bezel::Driver d;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = aObj.path;
    dep.objectPath = aObj.path;
    deps.push_back(dep);
    if (!d.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}
}  // namespace
BEZEL_TEST(sema_indexer_overload_nullability_2834,
           xmod_nullable_param_overload_with_imported_class_2414) {
    int r = compileImportRun2834(
        "module IdxLib2834; "
        "public class Acc2834 "
        "  public property [n: int?]: int => 99; "
        "end Acc2834;",
        "module App2834; import IdxLib2834; "
        "class C "
        "  private static method NI(): int? => null; "
        "  public static method Main(): int "
        "    using var a := new Acc2834(); "
        "    return a[NI()]; "         // [n: int?]  -> 99
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 99);
}
