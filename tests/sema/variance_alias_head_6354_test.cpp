// fix #6354 (§27.8, §12.9, v0.338): the §12.9 composed variance position check
// SKIPPED a local generic TYPE-ALIAS head.
//
// #6348's `namedHeadArgVariance` resolved a generic head's own variance from
// exactly two sources: the current module's `TypeSymbol`s (`localTypes`) and the
// compiler built-ins (`isBuiltinTypeName`). A `type X<T> := …` declaration is a
// `TopDecl`-level `TypeAliasSymbol` — neither — so it composed to `Unknown` and
// `composedVarianceViolation` reports no violation for `Unknown`. Every position
// reached through an alias head was therefore SILENTLY ACCEPTED.
//
// This is NOT the defect #6351 covers: that one is a head IMPORTED from another
// module, and its `VarianceHeads` bundle carries only `{local types, imported
// types, import aliases}` — no local-alias set — so the two are disjoint and a
// local alias head is `Unknown` on either tree.
//
// §27.8 (v0.338) is unambiguous, twice over:
//   "A type alias is a pure synonym — it does not create a new type."
//   "Type aliases — generic or not — see through to the aliased type for
//    identity, equality, method dispatch, and overload resolution. They are a
//    naming convenience, not a separate type kind."
// and §12.9 requires the position composition and makes classes "always
// invariant". So `MyList<T>` IS `Array<T>` and `Getter<T>` IS `(T) -> ()`, and
// an `out T` that reaches either cannot be honoured.
//
// THE CONTRAST IS THE POINT. Every reject row below is the ALIAS spelling of a
// shape #6348 already rejects without an alias; the `*_non_alias_twin_*` rows
// pin that twin so the local/alias contrast cannot drift. A row that tested
// only the failing shape would be half the evidence — the bug is the DIFFERENCE
// between the two spellings, and a fix that simply rejected everything would
// pass such a row.
//
// The sound compositions are pinned too, and are what keeps this from being a
// blanket rejection: an alias of a COVARIANT head at an output slot, an alias of
// an interface's own bare parameter, a PHANTOM alias parameter, and an alias in
// a position that is already sound for the non-alias spelling. An unresolved
// head still yields `Unknown` and is still skipped, so an alias cannot make the
// check start guessing.

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

// --- issue repro 1: an alias of an INVARIANT class at a RETURN slot ---------
//
// #6354's first measured repro, verbatim. `Array` is a class, so §12.9 makes
// its slot "always invariant" and no marker survives it.

BEZEL_TEST(variance_alias_head_6354, out_t_under_invariant_class_alias_return_rejected) {
    expectRejected("type MyList<T> := Array<T>; "
                   "interface I<out T> method Get(): MyList<T>; end I; ",
                   "composed through a nested type constructor");
}

BEZEL_TEST(variance_alias_head_6354, out_t_under_array_return_non_alias_twin_rejected) {
    expectRejected("interface I<out T> method Get(): Array<T>; end I; ",
                  "composed through a nested type constructor");
}

// --- issue repro 2: the same alias at an INPUT slot -------------------------

BEZEL_TEST(variance_alias_head_6354, out_t_under_invariant_class_alias_input_rejected) {
    expectRejected("type MyList<T> := Array<T>; "
                   "interface I<out T> method Put(v: MyList<T>); end I; ",
                   "composed through a nested type constructor");
}

BEZEL_TEST(variance_alias_head_6354, out_t_under_array_input_non_alias_twin_rejected) {
    expectRejected("interface I<out T> method Put(v: Array<T>); end I; ",
                  "composed through a nested type constructor");
}

// --- issue repro 3: an alias of a CONTRAVARIANT function type --------------
//
// #6354's third repro. §12.9: "Function types are themselves variant —
// covariant in return type, contravariant in parameter types." `(T) -> ()` is
// contravariant in T, and a contravariant position inside a method RETURN slot
// stays contravariant, so `out T` cannot honour it.

BEZEL_TEST(variance_alias_head_6354, out_t_under_function_alias_return_rejected) {
    expectRejected("type Getter<T> := (T) -> (); "
                   "interface I<out T> method Get(): Getter<T>; end I; ",
                   "composed through a nested type constructor");
}

BEZEL_TEST(variance_alias_head_6354, out_t_under_function_return_non_alias_twin_rejected) {
    expectRejected("interface I<out T> method Get(): (T) -> (); end I; ",
                  "composed through a nested type constructor");
}

// --- the see-through FOLLOWS THE CHAIN, it does not take one hop ------------
//
// #6354 raises this explicitly. `A<T>` names `B<T>` and `B<T>` names
// `Array<T>`, so one hop would stop at `B` and answer `Unknown`. The
// implementation asks the same question on the body it reaches, so the chain is
// transparent and the answer is the same `Invariant` the one-hop spelling gives.

BEZEL_TEST(variance_alias_head_6354, out_t_under_chained_alias_rejected) {
    expectRejected("type A<T> := B<T>; type B<T> := Array<T>; "
                   "interface I<out T> method Get(): A<T>; end I; ",
                   "composed through a nested type constructor");
}

// A chain that ENDS at a covariant head must still be ACCEPTED — this is what
// distinguishes a see-through from a "reject any alias" shortcut.

BEZEL_TEST(variance_alias_head_6354, out_t_under_chained_alias_to_covariant_head_accepted) {
    expectAccepted("type A<T> := B<T>; type B<T> := IReadOnlyList<T>; "
                   "interface I<out T> method Get(): A<T>; end I; ");
}

// --- the sound compositions stay sound (the fix must not over-reject) -------

BEZEL_TEST(variance_alias_head_6354, out_t_under_covariant_head_alias_accepted) {
    // §12.9 names `IReadOnlyList` covariant, so the alias of it is too, and an
    // `out T` reaches it in a covariant slot.
    expectAccepted("type MyRo<T> := IReadOnlyList<T>; "
                   "interface I<out T> method Get(): MyRo<T>; end I; ");
}

BEZEL_TEST(variance_alias_head_6354, out_t_under_contravariant_head_alias_input_accepted) {
    // §12.9's own `IEvent<out T>` worked example, re-spelled through an alias:
    // T inside `(T) -> ()` is CONTRAVARIANT, and that function type is itself a
    // method PARAMETER (an input, contravariant), giving T a COVARIANT
    // position overall. `out T` is therefore honoured THROUGH the alias.
    //
    // The twin below pins the same shape without the alias, which #6348 already
    // accepts — so the two rows together are the contrast: resolving the alias
    // must not change the verdict, only make it reachable.
    expectAccepted("type ICon<T> := (T) -> (); "
                   "interface I<out T> method Register(h: ICon<T>); end I; ");
}

BEZEL_TEST(variance_alias_head_6354, out_t_under_contravariant_head_input_non_alias_twin_accepted) {
    expectAccepted("interface I<out T> method Register(h: (T) -> ()); end I; ");
}

BEZEL_TEST(variance_alias_head_6354, in_t_under_contravariant_head_alias_input_rejected) {
    // The `in` mirror of the row above, and the pair that proves the alias is
    // genuinely composed rather than skipped: contravariant-of-contravariant is
    // COVARIANT here, which `out` honours and `in` cannot. MEASURED on the
    // non-alias twin first — `interface I<in T> method Register(h: (T) -> ());`
    // is rejected too — so this row asserts the alias spelling MATCHES the
    // shape #6348 already decides, not a reading invented for the alias.
    expectRejected("type ICon<T> := (T) -> (); "
                   "interface I<in T> method Register(h: ICon<T>); end I; ",
                   "composed through a nested type constructor");
}

BEZEL_TEST(variance_alias_head_6354, in_t_under_covariant_head_alias_input_accepted) {
    // An alias of a COVARIANT head reached through an INPUT slot: covariant-of-
    // contravariant is CONTRAVARIANT, which `in T` honours. The second slot the
    // see-through has to get right — a covariant head is not automatically sound.
    expectAccepted("type Ro<T> := IReadOnlyList<T>; "
                   "interface I<in T> method Register(h: Ro<T>); end I; ");
}

BEZEL_TEST(variance_alias_head_6354, alias_to_bare_parameter_accepted) {
    // #6354 flags this as "a different question — there the alias body IS the
    // parameter". §27.8 makes `Id<T>` interchangeable with `T`, so at a return
    // slot it places T covariantly and `out T` is exactly right. The body is
    // read as the ALIAS'S OWN parameter, which needs no name-resolution
    // machinery (#6340/#6344 own that shape and are untouched here).
    expectAccepted("type Id<T> := T; interface I<out T> method Get(): Id<T>; end I; ");
}

BEZEL_TEST(variance_alias_head_6354, alias_phantom_parameter_accepted) {
    // A parameter the body never mentions puts NO constraint on the use site,
    // so `Phantom<T>` is compatible with a marker either way. This is the row
    // that fails if the see-through answers "invariant" for every alias.
    expectAccepted("type Phantom<T> := int; "
                   "interface I<out T> method Get(): Phantom<T>; end I; ");
}

BEZEL_TEST(variance_alias_head_6354, out_t_under_covariant_local_head_through_alias_accepted) {
    // An alias of a LOCALLY declared interface keeps that interface's marker:
    // the see-through reaches a `TypeSymbol` through the body and reads its
    // `GenericParamSymbol::variance`, exactly as the direct spelling does.
    expectAccepted("interface ICo<out T> method Get(): T; end ICo; "
                   "type MyCo<T> := ICo<T>; "
                   "interface I<out T> method Get(): MyCo<T>; end I; ");
}

// --- the qualified-spelling guard ------------------------------------------
//
// `resolveLocalAliasHead` matches only a single-part spelling, so `Outer.Nested<T>`
// is NEVER read as a top-level alias `Nested` — even when one exists and even
// when the two share a simple name. That is deliberate: it is the defect class
// that got #6351's commit REJECTED in review (its imported arm compared
// `parts.back()` and so resolved `Outer.Inner<T>` against an unrelated top-level
// `Inner`, newly REJECTING a spec-legal program).
//
// The two rows below are ONE program under two spellings, which is what makes
// the guard observable at all. §11.6 puts a nested type inside a CLASS, so the
// nested interface is declared in `Outer`'s body with the §13.3 visibility
// keyword §11.6 requires. `Outer.Nested` is `out T`, so a `Get()` returning it
// is sound; the top-level alias `Nested<T> := Array<T>` is invariant, so a
// `Get()` returning THAT is not. If the guard were absent, the qualified
// spelling would resolve through the alias and wrongly reject the first program.

BEZEL_TEST(variance_alias_head_6354, qualified_nested_head_not_resolved_as_top_level_alias) {
    // The top-level alias `Nested` exists and is invariant; the spelling is
    // `Outer.Nested`, so it names the nested COVARIANT interface and is sound.
    expectAccepted("type Nested<T> := Array<T>; "
                   "public class Outer<T> "
                   "  public interface Nested<out T> method Get(): T; end Nested; "
                   "end Outer; "
                   "interface I<out T> method Get(): Outer.Nested<T>; end I; ");
}

BEZEL_TEST(variance_alias_head_6354, bare_head_of_same_name_still_resolves_through_the_alias) {
    // The other half, and the reason the row above is worth having: the BARE
    // `Nested<T>` in the identical program DOES name the top-level alias, and is
    // correctly composed through it (§27.8's synonym property). Together the two
    // rows pin that the guard keys on the SPELLING, not on the simple name.
    expectRejected("type Nested<T> := Array<T>; "
                   "public class Outer<T> "
                   "  public interface Nested<out T> method Get(): T; end Nested; "
                   "end Outer; "
                   "interface I<out T> method Get(): Nested<T>; end I; ",
                   "composed through a nested type constructor");
}

// --- two rows an earlier draft of this file asserted, and why ---------------
//
// `in_t_under_contravariant_head_alias_input_accepted` asserted that an `in T`
// under a contravariant head at an input slot is honoured. Contravariant-of-
// contravariant is COVARIANT, which `out` honours and `in` cannot — and the
// NON-ALIAS twin is rejected too, so the row asserted a verdict the compiler has
// never produced. It is now the reject row above, which matches the twin.
//
// `unresolved_head_still_skipped` asserted that a head naming nothing is skipped.
// `interface I<out T> method Get(): NotDeclared<T>;` is a hard "unresolved type
// name" from §2.2, never a skip. There is no head that is both unresolvable and
// accepted, so there is nothing to pin.
//
// The `Unknown` arms that remain reachable are the defensive ones (an alias with
// no declaration, a use site supplying more arguments than the alias declares, a
// chain past the depth cap). The last of those IS observable — a chain of 33 or
// more aliases ending at an invariant head composes to `Unknown` and is silently
// accepted — and it is a fail-safe under-approximation: it can never cause a
// false rejection, only a false accept, and only past 32 hand-written aliases.
// It is tracked as a follow-up rather than pinned here, because pinning it would
// enshrine the cap as the specified behaviour.

// A variance marker ON an alias parameter is its own §12.9 error, reported
// before this check and unchanged by it. Pinned so a future refactor that reads
// a marker off the alias cannot be mistaken for this fix.

BEZEL_TEST(variance_alias_head_6354, marker_on_alias_parameter_still_rejected) {
    expectRejected("type Marked<out T> := Array<T>; "
                   "interface I<out T> method Get(): T; end I; ",
                   "variance markers are valid only on an interface's own type "
                   "parameters");
}