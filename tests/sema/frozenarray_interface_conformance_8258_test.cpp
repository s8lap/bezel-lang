// fix #8258 (spec §5.1 *Implicit conversions*, stdlib §4.19, v0.355):
// an ANNOTATED `FrozenArray<int>` was admitted at an `IReadOnlyList<int>`
// parameter, and the call RAN.
//
// stdlib §4.19 (v0.355) declares `public class FrozenArray<T>: IEnumerable<T>`
// — that is its ONLY interface, and it declares no `Count` member (it has
// `Length`, `[i]` and `ToArray`). Spec §5.1 (v0.355) lists exactly one
// class-to-interface reference conversion — "a type to an interface it
// implements or conforms to: classes by declaration or inheritance" — so a
// `FrozenArray<T>` converts to `IEnumerable<T>` and to NOTHING else.
//
// THE DEFECT. `ExprTC::variantGenericAssignable` (src/sema/ExprTypecheck.cpp)
// searches the builtin interface hierarchy for the target interface; when the
// search finds no candidate it fell through to a top-level conservative
// ACCEPT. That fallback is sound only where the source's interface set is
// NOT completely modeled. `FrozenArray<T>`'s interface set IS completely
// modeled — `builtinInterfaceAncestors` has the `FrozenArray` -> `IEnumerable<T>`
// row (added by #8103) — but `FrozenArray` was absent from the
// `builtinBorrowingCollectionFullyModeled` predicate that gates the fallback,
// so the search's not-found was never read as PROOF of non-conformance and
// `FrozenArray<int>` was admitted at `IReadOnlyList<int>`.
//
// The fix adds `FrozenArray` to that predicate — the same one-line table
// entry #1851 added for `Array`/`Set`/`Stack`/… — so the not-found candidate
// now REJECTs.
//
// ORACLE: `runAot` (the AOT pipeline). The reject rows assert the SPECIFIC
// §5.1 diagnostic; the accept rows assert the program's stdout, so a row
// cannot pass by accident.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The §5.1 conversion diagnostic — "… has a type that is not implicitly
// convertible to the target type (§5.1)". Asserted by TEXT (not by the mere
// presence of an error) so a reject for an unrelated reason cannot pass.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// The program's captured stdout with ONE trailing line-ending stripped — the
// same normalisation `runAotOut` performs (Console.WriteLine appends the
// platform-dependent Console.NewLine), so the expectation is platform-
// independent.
std::string printed(const JitResult& r) {
    std::string s = r.stdoutText;
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }
    return s;
}

// The source of every row: an ANNOTATED `FrozenArray<int>` local (the
// declaration-less generic Named shape — stdlib §4.19's `FrozenArray<T>` as
// written in a type position).
const std::string kFrozenSource =
    "    using var a: Array<int> := [1, 2, 3];\n"
    "    using var g: FrozenArray<int> := FrozenArray<int>.From(a);\n";

// The issue's literal repro: the raise row. `FrozenArray<int>` is NOT an
// `IReadOnlyList<int>` (stdlib §4.19 declares only `IEnumerable<T>`), so
// spec §5.1 lists no conversion and the call must be REJECTED.
//
// WITHOUT the fix this program COMPILES and prints `3` (the over-acceptance
// the issue reports).
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_to_ireadonlylist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyList<int>): int => xs.Count;\n"
        "  public static method Main(): int\n" +
        kFrozenSource +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// The same conversion at an ASSIGNMENT position (not a call argument) — the
// §5.1 gate is shared, so the position must not decide the verdict.
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_to_ireadonlylist_initializer_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kFrozenSource +
        "    var x: IReadOnlyList<int> := g;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// The SIBLING over-acceptance closed by the same one-line table entry:
// `IReadOnlyCollection<int>` is equally NOT declared by stdlib §4.19, so the
// same §5.1 reasoning rejects it. (This row was ACCEPTED on the base tip.)
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_to_ireadonlycollection_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyCollection<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kFrozenSource +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `IList<int>` (stdlib §4.1, the borrowing mutable list) is not declared by
// stdlib §4.19 either.
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_to_ilist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IList<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kFrozenSource +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// The UNANNOTATED twin (`var g := FrozenArray<int>.From(a);` — the factory
// result carries the synthetic `builtinClassSym("FrozenArray")` stub rather
// than the declaration-less annotation shape). It was already REJECTED on the
// base tip; the fix must not change it.
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_unannotated_to_ireadonlylist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyList<int>): int => xs.Count;\n"
        "  public static method Main(): int\n"
        "    using var a: Array<int> := [1, 2, 3];\n"
        "    using var g := FrozenArray<int>.From(a);\n"
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// THE ADMITTING TWIN — the positive control that pins the fix to the exact
// boundary: `FrozenArray<int>` DOES conform to `IEnumerable<int>` (stdlib
// §4.19 declares it), so §5.1 converts it and this call must still compile
// and run. A fix that rejected every interface would fail here.
//
// The body does not enumerate the sequence THROUGH the interface: an
// `IEnumerable<T>`-typed view of a `FrozenArray<T>` crashes in the enumerator
// dispatch — a SEPARATE, pre-existing codegen defect, filed as its own issue
// (see the file header's note), not a §5.1 conversion question.
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_to_ienumerable_argument_accepted) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IEnumerable<int>): int => 42;\n"
        "  public static method Main(): int\n" +
        kFrozenSource +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("42"));
}

// THE ADMITTING TWIN OF THE OTHER FAMILY — `Array<int>` DOES declare
// `IList<T>` (stdlib §4.4), and `IList<T>` extends `IReadOnlyList<T>`
// (stdlib §4.1), so `Array<int>` -> `IReadOnlyList<int>` is a §5.1
// conversion and must still compile.
//
// THIS ROW READS THE DATA THROUGH THE CONVERTED INTERFACE (`xs.Count` is the
// interface's own member), so a fix that admitted the wrong value — or that
// mis-lowered the upcast — cannot pass it.
BEZEL_TEST(frozenarray_interface_conformance_8258,
           array_to_ireadonlylist_argument_accepted_reads_data) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyList<int>): int => xs.Count;\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1, 2, 3];\n"
        "    var g: Array<int> := a;\n"
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("3"));
}

// The `Object` root (spec §5.1's "any type to `Object`") — a `FrozenArray<int>`
// is a reference type and must still convert.
BEZEL_TEST(frozenarray_interface_conformance_8258,
           frozenarray_to_object_argument_accepted) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(o: Object): int => 7;\n"
        "  public static method Main(): int\n" +
        kFrozenSource +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("7"));
}

}  // namespace
