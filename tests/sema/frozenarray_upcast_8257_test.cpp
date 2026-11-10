// fix #8257 (spec §5.1 *Implicit conversions*, stdlib §4.19, v0.355):
// a `FrozenArray<int>` converted to `IEnumerable<int>` was NOT usable — the
// ANNOTATED twin compiled and then SEGVed when the callee enumerated it, and
// the INFERRED twin was REJECTED at compile time.
//
// stdlib §4.19 (v0.355) declares `public class FrozenArray<T>: IEnumerable<T>`
// — that is its ONLY interface. Spec §5.1 (v0.355) lists exactly one
// class-to-interface reference conversion — "a type to an interface it
// implements or conforms to: classes by declaration or inheritance" — so a
// `FrozenArray<T>` converts to `IEnumerable<T>` and to NOTHING else.
//
// THE TWO HALVES OF ONE HOLE.
//
//  * The CODEGEN half: §29.2's wrap grid has a SOURCE-FAMILY axis (the
//    collection heads that take the ARRAY-ENUMERATOR wrap). `FrozenArray` was
//    missing from it, so a `FrozenArray<T>` widened to `IEnumerable<T>` was
//    handed over as ITSELF — the raw `bz_array_t*` — where a
//    vtable-backed enumerator was expected. The callee's for-in then read the
//    array's first word as an `IEnumerable<T>` vtable and jumped where the
//    slot pointed: `AddressSanitizer: SEGV on unknown address ... in
//    bz$M.C$Sum`. The registry's wrap-source predicate is name-keyed, and
//    the stdlib §4.19 `From(...)` factory-result shape carries NO name that
//    `headName()` can read (it is a `builtinClassSym` stub: a non-null
//    declaration with no TypeDecl, no module, no `astNode` and no
//    `importedName`), so BOTH twins need the dual-shape read.
//
//  * The SEMANTIC half: the two twins are the SAME nominal type, but they
//    are DIFFERENT `TypeRef` NODES. The ANNOTATED one (`using var g:
//    FrozenArray<int> := …`) is a declaration-less Named with an `astNode`;
//    the INFERRED one (`using var f := FrozenArray<int>.From(a)`) is the
//    factory's return type, stamped `declaration =
//    builtinClassSym("FrozenArray")`. The §5.1 gate asked the USER-declared
//    base-conformance walk about the second shape, whose declared-base list
//    is empty (it is a compiler-intrinsic TypeSymbol, not a source
//    declaration) — so it found no base and REJECTED a conversion stdlib
//    §4.19 declares, while the annotation twin was admitted.
//
// Neither half alone closes the issue: the codegen half leaves the inferred
// twin rejected, and the semantic half leaves the annotated twin crashing.
//
// THE TWINS GET ONE SOURCE EACH. Every accepting row below declares ONLY the
// twin it names. A source that declared both twins would fail as a WHOLE on
// the unfixed compiler — the inferred twin's compile-time rejection takes the
// program down with it — so the annotated twin's row would fail for the
// WRONG reason (a compile error instead of the SEGV). With one twin per
// source each row's failure on the unfixed compiler is unambiguous: the
// ANNOTATED row fails on the SEGV, the INFERRED row fails on the
// "not implicitly convertible to the target type (§5.1)" diagnostic.
//
// ORACLE: `runAot` (the AOT pipeline, compiled and linked with
// AddressSanitizer). The accept rows assert the program's stdout, so a row
// cannot pass by accident; the reject rows assert the SPECIFIC §5.1
// diagnostic by TEXT, so a rejection for an unrelated reason cannot pass.

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

// A callee that ENUMERATES its `IEnumerable<int>` parameter — the shape that
// SEGVed before the fix. It reads the DATA (not just the length), so a row
// cannot pass by accident: a wrong value would print the wrong number.
const std::string kSumCtor =
    "  static method Sum(xs: IEnumerable<int>): int\n"
    "    var s := 0;\n"
    "    for var x in xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n";

// The ANNOTATED twin ALONE — the declaration-less generic Named shape (the
// `using var g: FrozenArray<int> := …` local). Its own source, so the row
// that uses it fails on the unfixed compiler with the SEGV and nothing else.
const std::string kAnnotatedTwin =
    "    using var a: Array<int> := [1, 2, 3];\n"
    "    using var g: FrozenArray<int> := FrozenArray<int>.From(a);\n";

// The INFERRED twin ALONE — the stdlib §4.19 `From(...)` factory result, the
// `builtinClassSym` stub shape (the `using var f := …` local). Its own
// source, so the row that uses it fails on the unfixed compiler with the
// §5.1 over-rejection and nothing else.
const std::string kInferredTwin =
    "    using var a: Array<int> := [1, 2, 3];\n"
    "    using var f := FrozenArray<int>.From(a);\n";

// ---------------------------------------------------------------------------
// THE TWO TWINS — each in its own source, each accepting the §5.1 conversion
// stdlib §4.19 declares.
// ---------------------------------------------------------------------------

// The ANNOTATED twin at an `IEnumerable<int>` PARAMETER: the issue's literal
// repro (a). It compiled and then SEGVed in the callee.
//
// WITHOUT the fix: compiles, the program SEGVs — `AddressSanitizer: SEGV ...
// in bz$M.C$Sum` — and prints nothing.
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_to_ienumerable_parameter_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n" +
        kSumCtor +
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(Sum(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("6"));
}

// The INFERRED twin at the same parameter: the issue's repro (b). It was
// REJECTED at compile time ("not implicitly convertible to the target type
// (§5.1)") although the annotated twin was accepted.
//
// WITHOUT the fix: the program does not compile at all.
BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_to_ienumerable_parameter_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n" +
        kSumCtor +
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    Console.WriteLine(Sum(f));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("6"));
}

// The ANNOTATED twin BOUND to an `IEnumerable<int>` LOCAL: the issue's p27.
// The local held the raw `bz_array_t*` and the for-in vtable-dispatched on
// the array's first word.
//
// WITHOUT the fix: compiles, the program SEGVs — `AddressSanitizer: SEGV ...
// in main`.
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_bound_to_ienumerable_local_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    var e: IEnumerable<int> := g;\n"
        "    var s := 0;\n"
        "    for var x in e do s := s + x; end;\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("6"));
}

// The INFERRED twin BOUND to an `IEnumerable<int>` LOCAL: the issue's p5 row.
// Before the fix this position was REJECTED at compile time (the same
// over-rejection as the argument position); after it, the local must hold a
// vtable-backed enumerator, not the raw `bz_array_t*`.
//
// WITHOUT the fix: the program does not compile at all.
BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_bound_to_ienumerable_local_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    var e: IEnumerable<int> := f;\n"
        "    var s := 0;\n"
        "    for var x in e do s := s + x; end;\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("6"));
}

// The GENERIC callee: `Cnt<T>(xs: IEnumerable<T>)` with an annotated
// `FrozenArray<int>` argument (the issue's p26). The type-argument
// instantiation must still enumerate the elements.
//
// WITHOUT the fix: compiles, the program SEGVs — `AddressSanitizer: SEGV ...
// in bz$M.C$Cnt`.
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_to_generic_ienumerable_parameter_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method Cnt<T>(xs: IEnumerable<T>): int\n"
        "    var n := 0;\n"
        "    for var x in xs do n := n + 1; end;\n"
        "    return n;\n"
        "  end Cnt;\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(Cnt(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("3"));
}

// ---------------------------------------------------------------------------
// THE NON-CONFORMANCE ROWS — stdlib §4.19 declares `FrozenArray<T>`'s ONLY
// interface to be `IEnumerable<T>`, so every one of these four targets must
// still be REJECTED (§5.1: a class conforms to an interface only by
// DECLARATION or INHERITANCE, and `FrozenArray<T>` declares none of them).
//
// These rows are the guard against a fix that "widens the conformance
// search": the search must find `IEnumerable<T>` and NOTHING else, for BOTH
// twins. Each row asserts the §5.1 diagnostic by TEXT.
// ---------------------------------------------------------------------------

// `IList<int>` (stdlib §4.1, the borrowing mutable list).
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_to_ilist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IList<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `IReadOnlyList<int>` (stdlib §4.1).
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_to_ireadonlylist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyList<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `IReadOnlyCollection<int>` (stdlib §4.1).
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_to_ireadonlycollection_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyCollection<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `ICollection<int>` (stdlib §4.1).
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_to_icollection_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: ICollection<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// The INFERRED twin at the same four targets — the over-rejection fix must
// not have WIDENED the search: the inferred twin is rejected for exactly the
// same reason as the annotated twin (stdlib §4.19 declares only
// `IEnumerable<T>`).
BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_to_ilist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IList<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    Console.WriteLine(N(f));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_to_ireadonlylist_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyList<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    Console.WriteLine(N(f));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_to_ireadonlycollection_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: IReadOnlyCollection<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    Console.WriteLine(N(f));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_to_icollection_argument_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(xs: ICollection<int>): int => 0;\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    Console.WriteLine(N(f));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// ---------------------------------------------------------------------------
// THE VARIANCE ROWS — stdlib §4.19 declares `FrozenArray<T>: IEnumerable<T>`,
// and stdlib §5 declares `public interface IEnumerable<out T>` — COVARIANT
// (spec §12.9). So the element type must be IDENTITY for a VALUE type (a
// value type admits identity only) but may widen by REFERENCE conversion for
// a reference type.
//
//  * `FrozenArray<int>` -> `IEnumerable<Object>` — the element type `int` is
//    a VALUE type, so §12.9 admits identity only, and `int` is not `Object`:
//    the conversion is REFUSED (§5.1).
//
//  * `FrozenArray<Dog>` -> `IEnumerable<Animal>` — the element type `Dog` is
//    a REFERENCE type and `Dog` converts to `Animal` by inheritance, so the
//    covariant conversion is SOUND and is ADMITTED. This row is the twin
//    parity row: the ANNOTATED twin was already admitted (it crashed for the
//    unrelated §29.2 wrap reason the fix also repairs), and the INFERRED
//    twin is admitted by the same conformance facts — the very property this
//    fix restores ("the inferred type carries the same conformance facts as
//    the annotated one").
//
// The first pair are the guard against a fix that lets the §12.9 variance
// search accept a mismatched value-type instantiation.
// ---------------------------------------------------------------------------

BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_int_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    var e: IEnumerable<Object> := g;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_int_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    var e: IEnumerable<Object> := f;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// The ANNOTATED twin at the covariant target: ADMITTED, and the elements are
// enumerated (the §29.2 wrap applies here too).
//
// WITHOUT the fix: compiles, the program SEGVs — `AddressSanitizer: SEGV ...
// in bz$M.C$Cnt`.
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_twin_dog_to_ienumerable_animal_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class Animal\n"
        "end Animal;\n"
        "class Dog: Animal\n"
        "end Dog;\n"
        "class C\n"
        "  static method Cnt(xs: IEnumerable<Animal>): int\n"
        "    var n := 0;\n"
        "    for var x in xs do n := n + 1; end;\n"
        "    return n;\n"
        "  end Cnt;\n"
        "  public static method Main(): int\n"
        "    using var a: Array<Dog> := [new Dog()];\n"
        "    using var g: FrozenArray<Dog> := FrozenArray<Dog>.From(a);\n"
        "    Console.WriteLine(Cnt(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("1"));
}

// The INFERRED twin at the SAME covariant target: the same conformance facts
// as the annotated twin above, so it is admitted and enumerates too. Before
// the fix this position was REJECTED at compile time — the over-rejection the
// issue is about.
//
// WITHOUT the fix: the program does not compile at all.
BEZEL_TEST(frozenarray_upcast_8257,
           inferred_twin_dog_to_ienumerable_animal_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class Animal\n"
        "end Animal;\n"
        "class Dog: Animal\n"
        "end Dog;\n"
        "class C\n"
        "  static method Cnt(xs: IEnumerable<Animal>): int\n"
        "    var n := 0;\n"
        "    for var x in xs do n := n + 1; end;\n"
        "    return n;\n"
        "  end Cnt;\n"
        "  public static method Main(): int\n"
        "    using var a: Array<Dog> := [new Dog()];\n"
        "    using var fd := FrozenArray<Dog>.From(a);\n"
        "    Console.WriteLine(Cnt(fd));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("1"));
}

// ---------------------------------------------------------------------------
// THE BUILTIN-STUB ROWS — the sound-reject rows for the OTHER builtin value
// types.
//
// The §5.1 gate consults the declared-base conformance walk for a source
// that carries a DECLARATION. A builtin VALUE type reaches this site in the
// `builtinClassSym` STUB shape (a non-null declaration with no TypeDecl and
// no module), and its declared-base closure is EMPTY — so the walk finds no
// base and the conversion is REFUSED.
//
// These five rows pin that refusal for `DateTime`, `Guid`, `Stopwatch`,
// `TimeSpan` and `CancellationToken`. None of them conforms to
// `IEnumerable<T>`, so §5.1 must refuse every one of them.
//
// They are the rows that catch a fix which widens the §5.1 gate for the whole
// builtin-stub class: such a fix SILENTLY ADMITS these five at
// `IEnumerable<Object>` (an unsound accept — a later interface call
// dispatches through an itable the object does not have).
// ---------------------------------------------------------------------------

// `DateTime` — stdlib §2.5, a value type with no interface base.
BEZEL_TEST(frozenarray_upcast_8257,
           datetime_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IEnumerable<Object> := DateTime.Now;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `Guid` — stdlib §2.6, a value type with no interface base.
BEZEL_TEST(frozenarray_upcast_8257,
           guid_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IEnumerable<Object> := Guid.NewGuid();\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `Stopwatch` — stdlib §8.1 (`System.Diagnostics`), a value type with no
// interface base.
BEZEL_TEST(frozenarray_upcast_8257,
           stopwatch_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "import System.Diagnostics;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IEnumerable<Object> := Stopwatch.StartNew();\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `TimeSpan` — stdlib §2.5, a value type with no interface base.
BEZEL_TEST(frozenarray_upcast_8257,
           timespan_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IEnumerable<Object> := TimeSpan.Zero;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `CancellationToken` — stdlib §6.1, a value type with no interface base.
BEZEL_TEST(frozenarray_upcast_8257,
           cancellationtoken_to_ienumerable_object_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IEnumerable<Object> := CancellationToken.None;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// ---------------------------------------------------------------------------
// THE `Range<T>` ROWS — `Range<T>` is a compiler-intrinsic record that
// implements `IEnumerable<T>` and NOTHING else (spec §16.3). It is NOT one of
// the borrowing-collection families whose complete interface set the variance
// search models, so the §12.9 search cannot PROVE non-conformance for it and
// the declared-base walk must still refuse it.
//
// These four rows pin that refusal at the four collection interfaces
// `Range<T>` does not implement. They are the rows that catch a fix which
// lets `Range<T>` through: such a fix does not merely ACCEPT the conversion,
// it accepts it into CODEGEN, where the raw range record is handed over
// where a vtable-backed enumerator is expected and the compiler emits
// MALFORMED IR ("Call parameter type does not match function signature!",
// then "codegen error: IR verification failed") — a COMPILER CRASH on a
// program that the §5.1 refusal turned away cleanly.
//
// Each row asserts BOTH halves of the refusal: the program did NOT run (no
// binary was produced) AND the diagnostic is the §5.1 one.
// ---------------------------------------------------------------------------

// `Range<int>` -> `IReadOnlyList<int>` (the range literal in a local
// declaration's initializer).
BEZEL_TEST(frozenarray_upcast_8257,
           range_to_ireadonlylist_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IReadOnlyList<int> := 1..3;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `Range<int>` -> `IList<int>`.
BEZEL_TEST(frozenarray_upcast_8257,
           range_to_ilist_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IList<int> := 1..3;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `Range<int>` -> `ICollection<int>`.
BEZEL_TEST(frozenarray_upcast_8257,
           range_to_icollection_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: ICollection<int> := 1..3;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// `Range<int>` -> `IReadOnlyCollection<int>`.
BEZEL_TEST(frozenarray_upcast_8257,
           range_to_ireadonlycollection_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: IReadOnlyCollection<int> := 1..3;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// The SAME four refusals with the range in an ANNOTATED `Range<int>` local
// (the declaration-less Named shape) rather than a range literal. The two
// shapes are different `TypeRef` nodes, so both are pinned.
BEZEL_TEST(frozenarray_upcast_8257,
           annotated_range_to_ireadonlylist_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var r: Range<int> := 1..3;\n"
        "    var e: IReadOnlyList<int> := r;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           annotated_range_to_ilist_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var r: Range<int> := 1..3;\n"
        "    var e: IList<int> := r;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           annotated_range_to_icollection_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var r: Range<int> := 1..3;\n"
        "    var e: ICollection<int> := r;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

BEZEL_TEST(frozenarray_upcast_8257,
           annotated_range_to_ireadonlycollection_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var r: Range<int> := 1..3;\n"
        "    var e: IReadOnlyCollection<int> := r;\n"
        "    Console.WriteLine(e);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// ---------------------------------------------------------------------------
// THE REGRESSION ROWS — the other families' upcasts must be UNCHANGED.
// ---------------------------------------------------------------------------

// The ARRAY twin: `Array<int>` -> `IEnumerable<int>` at a parameter, the
// §29.2 array-enumerator wrap's own home. It must still enumerate the data.
BEZEL_TEST(frozenarray_upcast_8257,
           array_twin_to_ienumerable_parameter_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n" +
        kSumCtor +
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1, 2, 3];\n"
        "    Console.WriteLine(Sum(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("6"));
}

// The ARRAY twin BOUND to an `IEnumerable<int>` local — the other face of the
// same §29.2 wrap (the local-decl site).
BEZEL_TEST(frozenarray_upcast_8257,
           array_twin_bound_to_ienumerable_local_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [1, 2, 3];\n"
        "    var e: IEnumerable<int> := a;\n"
        "    var s := 0;\n"
        "    for var x in e do s := s + x; end;\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("6"));
}

// The STACK twin: `Stack<int>` -> `IEnumerable<int>`. stdlib §4.11 fixes its
// iteration order (top-to-bottom, pop order), so the row asserts THAT order
// — the wrap's reverse machinery must still be in force.
BEZEL_TEST(frozenarray_upcast_8257,
           stack_twin_to_ienumerable_parameter_enumerates_in_pop_order) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method Sum(xs: IEnumerable<int>): int\n"
        "    var s := 0;\n"
        "    for var x in xs do s := s * 10 + x; end;\n"
        "    return s;\n"
        "  end Sum;\n"
        "  public static method Main(): int\n"
        "    using var st: Stack<int> := [1, 2, 3];\n"
        "    Console.WriteLine(Sum(st));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("321"));
}

// The QUEUE twin: `Queue<int>` -> `IEnumerable<int>` (stdlib §4.11, FIFO).
BEZEL_TEST(frozenarray_upcast_8257,
           queue_twin_to_ienumerable_parameter_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method Sum(xs: IEnumerable<int>): int\n"
        "    var s := 0;\n"
        "    for var x in xs do s := s * 10 + x; end;\n"
        "    return s;\n"
        "  end Sum;\n"
        "  public static method Main(): int\n"
        "    using var q: Queue<int> := [1, 2, 3];\n"
        "    Console.WriteLine(Sum(q));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("123"));
}

// The SET twin: `Set<int>` -> `IEnumerable<int>`. stdlib §4.10's set is
// HASH-backed, so it is SNAPSHOTTED rather than wrapped — the count is
// asserted (not the order, which the hash table does not fix).
BEZEL_TEST(frozenarray_upcast_8257,
           set_twin_to_ienumerable_parameter_enumerates) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method Cnt(xs: IEnumerable<int>): int\n"
        "    var n := 0;\n"
        "    for var x in xs do n := n + 1; end;\n"
        "    return n;\n"
        "  end Cnt;\n"
        "  public static method Main(): int\n"
        "    using var st: Set<int> := [1, 2, 3];\n"
        "    Console.WriteLine(Cnt(st));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("3"));
}

// The `Object` root (spec §5.1's "any type to `Object`") — a `FrozenArray<int>`
// is a reference type and must still convert. The ANNOTATED twin.
BEZEL_TEST(frozenarray_upcast_8257,
           frozenarray_to_object_argument_accepted) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(o: Object): int => 7;\n"
        "  public static method Main(): int\n" +
        kAnnotatedTwin +
        "    Console.WriteLine(N(g));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("7"));
}

// The `Object` root — the INFERRED twin. The same conversion, through the
// stub shape.
BEZEL_TEST(frozenarray_upcast_8257,
           inferred_frozenarray_to_object_argument_accepted) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method N(o: Object): int => 7;\n"
        "  public static method Main(): int\n" +
        kInferredTwin +
        "    Console.WriteLine(N(f));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The twin's failure on an UNFIXED compiler must be the RIGHT one: an
    // accepting row may never fail because of the §5.1 over-rejection (that
    // is the INFERRED twin's symptom), so assert the over-rejection
    // diagnostic is ABSENT. On an unfixed compiler this row then fails on
    // its own symptom alone — the ANNOTATED twin's crash
    // ("program terminated by signal 11") — and the INFERRED twin's row
    // fails on this very assertion. That is what makes the two twins'
    // failures UNAMBIGUOUS.
    BEZEL_EXPECT_FALSE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(printed(r), std::string("7"));
}

}  // namespace
