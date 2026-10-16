// fix #6348 (§12.9, v0.338): the variance POSITION check composes through
// nested type constructors — it used to read only a bare `T`.
//
// `Resolver::checkVariancePositions` tested each member type through
// `isDirectParamRef`, which fires only for a single-part QualifiedName with no
// generic args. Its own comment recorded the gap: "(Nested positions inside
// function types or generic args are skipped.)" §12.9 makes composition
// normative — "Position is composed across nested type constructors. A type
// parameter inside another generic type inherits position from that wrapper's
// variance … Function types are themselves variant (covariant in return,
// contravariant in parameter) … The compiler enforces position constraints" —
// so every nested occurrence below was silently accepted.
//
// THE FIX (src/sema/TypeResolution_Checks.cpp): `composedParamPosition` walks
// `ast::Type` and composes the position outward — a generic argument through
// the head's OWN variance (its `GenericParamSymbol::variance` for a declared
// type, the nine covariant stdlib built-ins otherwise, Invariant for every
// other built-in head since §12.9 makes classes "always invariant"), a
// function type through its return (covariant) and parameter (contravariant)
// slots, and an inline-array element through an invariant slot. A head the
// pass cannot resolve yields `Unknown` and is skipped rather than guessed, so
// the check stays free of false positives. The base list is composed the same
// way ("The extending interface instantiates the base's variant parameter with
// its own type parameter, holding the deriving interface's variance").
//
// The sound compositions are pinned too: the spec's own `IEvent<out T>` worked
// example (contra-of-contra = co) must KEEP compiling, and so must a covariant
// head reached through a covariant slot.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A trivial entry point so `runAot` reaches type checking; the programs under
// test are rejected at the interface declaration, long before codegen.
constexpr const char* kMain =
    "class Program "
    "  public static method Main() Console.WriteLine(\"x\"); end Main; "
    "end Program; ";

std::string program(const std::string& decls) {
    return "module M; import System; " + decls + kMain;
}

// REJECT: the declared marker cannot be honoured at the composed position.
void expectRejected(const std::string& decls, const char* snippet) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, snippet));
}

// ACCEPT: the composition is sound (or the parameter is never used), so the
// declaration must compile clean.
void expectAccepted(const std::string& decls) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// --- the invariant-wrapper family (§12.9: classes are "always invariant") ---

BEZEL_TEST(variance_position_composed_6348, out_t_under_array_in_parameter_rejected) {
    expectRejected("interface I<out T> method Put(xs: Array<T>); end I; ",
                  "composed through a nested type constructor");
}

BEZEL_TEST(variance_position_composed_6348, out_t_under_array_in_return_rejected) {
    expectRejected("interface I<out T> method Get(): Array<T>; end I; ",
                  "composed through a nested type constructor");
}

BEZEL_TEST(variance_position_composed_6348, in_t_under_array_in_return_rejected) {
    expectRejected("interface I<in T> method Make(): Array<T>; end I; ",
                  "composed through a nested type constructor");
}

BEZEL_TEST(variance_position_composed_6348, out_t_under_dictionary_in_parameter_rejected) {
    expectRejected(
        "interface I<out T> method Put(d: Dictionary<T, int>); end I; ",
        "composed through a nested type constructor");
}

// --- function types (§12.9: covariant return, contravariant parameter) -------

BEZEL_TEST(variance_position_composed_6348, out_t_in_function_return_parameter_rejected) {
    // `() -> T` is covariant in T; sitting in a method parameter (an input
    // slot) that composes to CONTRAVARIANT, which `out T` cannot honour.
    expectRejected("interface I<out T> method Register(f: () -> T); end I; ",
                  "composed through a nested type constructor");
}

BEZEL_TEST(variance_position_composed_6348, in_t_in_function_return_parameter_accepted) {
    // The mirror of the row above: contravariant-of-contravariant reads as
    // covariant here, so `in T` is exactly right.
    expectAccepted("interface I<in T> method Register(f: () -> T); end I; ");
}

BEZEL_TEST(variance_position_composed_6348, event_out_t_nested_function_accepted) {
    // §12.9's own worked example, unchanged from the pre-#6348 lit row
    // `sema_variance_position__spec_12_9_event_out_T_nested_function_accepted`:
    // T inside the parameter of `(T) -> ()`, itself a method parameter, is
    // contravariant-of-contravariant = COVARIANT, so `out T` is sound.
    expectAccepted("interface IEvent<out T> method Subscribe(h: (T) -> ()); end IEvent; ");
}

// --- a locally declared head's own marker is read --------------------------

BEZEL_TEST(variance_position_composed_6348, out_t_under_local_covariant_head_accepted) {
    expectAccepted(
        "interface ICo<out T> method Get(): T; end ICo; "
        "interface I<out T> method Get(): ICo<T>; end I; ");
}

BEZEL_TEST(variance_position_composed_6348, out_t_under_local_contravariant_head_rejected) {
    expectRejected(
        "interface ICon<in T> method Put(x: T); end ICon; "
        "interface I<out T> method Get(): ICon<T>; end I; ",
        "composed through a nested type constructor");
}

// --- base lists ("holding the deriving interface's variance") ---------------

BEZEL_TEST(variance_position_composed_6348, in_t_in_covariant_base_list_rejected) {
    expectRejected("interface Bad<in T>: IEnumerable<T> end Bad; ",
                  "whose own parameter variance cannot honour that position");
}

BEZEL_TEST(variance_position_composed_6348, out_t_in_covariant_base_list_accepted) {
    // The stdlib's own shape: IReadOnlyCollection<out T>: IEnumerable<T>.
    expectAccepted(
        "interface IROut<out T>: IEnumerable<T> "
        "  readonly property Count: int; "
        "end IROut; ");
}

// The base list DEFERS an ambiguity rather than picking a reading (narrow
// #6348; spec-question #6355). §12.9 opens the case with "A generic interface
// may extend a generic base whose variance is different from its own" and works
// it as `IList<T>` — invariant — extending `IReadOnlyList<out T>`, and closes
// with "The inheritance relation only fixes the type argument substituted into
// the base — it does not alter what positions T may legally appear in within
// the derived interface". Read together those withdraw the base list from the
// position check. Against them C# rejects the shape (CS1961), so the constraint
// reading has precedent and the text is not beyond doubt; #6355 records both.
//
// An INVARIANT base is the case deferred HERE — the spec's own blessed example
// is an invariant head, so it is evidence for acceptance, but a soundness
// argument survives and the text does not settle it. It yields `Unknown` and is
// skipped. These two rows pin the deferral: the shapes compile. If #6355 closes
// the other way they flip to `expectRejected` with the needle "whose own
// parameter variance cannot honour that position".
BEZEL_TEST(variance_position_composed_6348, base_invariant_head_deferred_spec_12_9) {
    expectAccepted(
        "interface IInv<T> method Put(x: T); end IInv; "
        "interface D<out T>: IInv<T> end D; ");
}

BEZEL_TEST(variance_position_composed_6348,
           base_invariant_head_deferred_contravariant_spec_12_9) {
    expectAccepted(
        "interface IInv<T> method Put(x: T); end IInv; "
        "interface D<in T>: IInv<T> end D; ");
}

// --- the direct, non-composed positions are unchanged (no false positives) --

BEZEL_TEST(variance_position_composed_6348, direct_out_return_still_accepted) {
    expectAccepted("interface I<out T> method Get(): T; end I; ");
}

BEZEL_TEST(variance_position_composed_6348, direct_in_parameter_still_accepted) {
    expectAccepted("interface I<in T> method Put(x: T); end I; ");
}