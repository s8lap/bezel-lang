// Spec-derived regression test for issue #2593.
//
// §7 (Nullability) / stdlib §5 (`System.Collections.Linq`), v0.237: stdlib §5
// declares the LINQ quantifier `Contains(value: T)` with a NON-nullable `T`
// on an `IEnumerable<T>` receiver. Like every builtin call with no
// MethodSymbol, it skipped the per-argument assignability check that
// checkCallArity -> checkArgsAgainstParams -> checkAssignable runs for user
// methods. #2582 closed that hole for the CONCRETE collection families, but
// its gate keyed on a recognized head name, and the two receiver shapes LINQ
// actually presents do not supply one:
//   (a) the LAZY sentinel — the result of a chained operator (`xs.Where(…)`)
//       — a Named TypeRef with neither a declaration nor an astNode, so the
//       head name comes out EMPTY and the gate bailed;
//   (b) a source-spelled `IEnumerable<T>` (and kin), whose head name is not a
//       concrete collection family.
// An un-narrowed `T?` argument therefore reached codegen, where the
// equality-compare lowering tripped an LLVM ICmpInst assertion
// ("Both operands to ICmp instruction are not of the same type!", exit 134) —
// the same failure mode #2582 documents. A user method `Contains(v: int)`
// with the identical signature correctly rejects the same argument (§7), so
// the two paths disagreed.
//
// This is a REJECTION fix, so each negative case asserts on diagnostic TEXT
// (not the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so it cannot distinguish a
// genuine reject from a crash; here the pre-fix behaviour WAS a crash, which
// is exactly the confusion the text assertion avoids). The positive cases
// guard against false rejection of working LINQ.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2593 / §7 / stdlib §5: the issue's repro — `Contains` on the LAZY receiver
// produced by a chained `Where(…)`, with an un-narrowed `int?`. Without the
// fix this ICE'd codegen (exit 134); with it, a clean sema reject.
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_contains_on_lazy_receiver_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(arr: Array<int>): bool "
        "    return arr.Where(lambda(x) => x > 1).Contains(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "IEnumerable<T>.Contains"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""7)"));
}

// #2593 / stdlib §5: the second receiver shape — a SOURCE-SPELLED
// `IEnumerable<T>` parameter. Its head name is a collection interface, not a
// concrete family, so #2582's name list did not reach it either.
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_contains_on_spelled_ienumerable_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(e: IEnumerable<int>): bool "
        "    return e.Contains(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "IEnumerable<T>.Contains"));
}

// #2593 / stdlib §5: the element type is whatever the sequence carries — a
// `string?` argument against an `IEnumerable<string>` is rejected the same
// way. (On the string path the pre-fix damage was a wrong answer rather than
// an ICE, since the runtime comparison helper guards null.)
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_contains_string_element_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): string? => null; "
        "  public static method T(e: IEnumerable<string>): bool "
        "    return e.Contains(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "IEnumerable<T>.Contains"));
}

// ---- Positive controls (guard against false rejection of working LINQ) ----

// #2593 / stdlib §5: a plain non-nullable argument on a lazy receiver
// compiles unchanged (positive baseline).
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_contains_accepts_plain_element) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(arr: Array<int>): bool "
        "    return arr.Where(lambda(x) => x > 1).Contains(2); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2593 / §7.4: a NARROWED `int?` is assignable to the non-nullable `value: T`
// — the narrowing exception lives inside checkAssignable and is reused
// unchanged, so sema must NOT reject this. (A separate CODEGEN defect on this
// exact shape — the LINQ Contains lowering does not unwrap the narrowed
// nullable and trips the same ICmpInst assertion — is filed as #2642; this
// test pins the SEMA half, which is that no §7 diagnostic is emitted.)
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_contains_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(arr: Array<int>): bool "
        "    var n: int? := 3; "
        "    if n = null then return false; end; "  // narrows n non-null below
        "    return arr.Where(lambda(x) => x > 0).Contains(n); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2593 / §7 + §7.10: when the SEQUENCE's element type is itself nullable —
// `IEnumerable<int?>` declares `Contains(value: int?)` — an un-narrowed
// `int?` is admissible. This is the case a naive "reject any nullable
// argument" gate breaks.
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_contains_accepts_nullable_element_sequence) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(e: IEnumerable<int?>): bool "
        "    return e.Contains(N()); "  // Contains(value: int?) — OK
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2593 / stdlib §5: the rest of the §5 operator surface takes predicates,
// selectors, `int` counts and other sequences — none of them element slots.
// The new pos-0 `Contains` rule must not mis-fire on any of them (this is the
// shape of the regression #2582 hit with LINQ `Take(count)`).
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           linq_non_contains_operators_are_not_element_slots) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(arr: Array<int>): int "
        "    var a := arr.Where(lambda(x) => x > 1).Count(); "
        "    var b := arr.Select(lambda(x) => x * 10).Count(); "
        "    var c := arr.Take(2).Count(); "
        "    var d := arr.Skip(1).Count(); "
        "    return a + b + c + d; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2593 / stdlib §4.4: a CONCRETE `Array<int>` receiver keeps going through
// #2582's element-collection path (head name "Array"), not the new
// IEnumerable branch — the diagnostic still names `Array<T>.Contains`, so the
// two branches are disjoint and neither double-reports.
BEZEL_TEST(sema_linq_contains_nullable_arg_2593,
           concrete_array_receiver_still_uses_the_element_collection_branch) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(arr: Array<int>): bool "
        "    return arr.Contains(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.Contains"));
    BEZEL_EXPECT_TRUE(!hasError(r, "IEnumerable<T>.Contains"));
}
