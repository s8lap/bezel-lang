// Spec-derived tests for the v0.235 iteration-variable ruling (#1666, #2381)
// — the bare-type-parameter callee-side admission of an iteration-variable
// borrow (spec §28.3/§29.1/§29.2, v0.235).
//
// §28.3 *Bare-type-parameter returns* — Callee side (v0.235, #1666): "an
// iteration-variable borrow is admissible only where its derived source chain
// reaches the receiver under §29.2's desugaring and §29.1's Current semantics
// at the loop source's STATIC type — a collection-backed source re-borrows
// (the chain runs element slot -> collection -> receiver), an enumerator-owned
// yield does not — and at an interface-typed enumeration source the backing is
// unprovable, so the variable's source is conservatively the loop's implicit
// enumerator, a scope-bound local, and returning it is rejected; the index-
// based spelling `this[i]`, on a receiver that declares an indexer, is
// receiver-sourced and admissible."
//
// The contract has two halves:
//   * MUST-REJECT — `extension IEnumerable<T> where T: class` whose body is
//     `for var item in this do return item; end;` (the pre-v0.235 §26.5.1
//     First() shape). The loop source is statically `IEnumerable<T>` (an
//     interface), so the element's backing is unprovable; the iteration
//     variable's source is the loop's implicit enumerator, a scope-bound local,
//     and returning it hands back a borrow that dies with the callee frame.
//   * ACCEPT — the index spelling `return this[0]` in `extension Array<T>`
//     (receiver-sourced); and a concrete collection-backed loop source
//     `for var x in this do return x; end;` in `extension Array<T>` (the chain
//     runs element slot -> collection -> receiver).
//
// The bug (#2381): the iteration-variable ruling was not enforced — a bare-T
// extension-method body that returned the iteration variable of an interface-
// typed enumeration was silently accepted (unsound). The root cause was twofold:
// (a) the ownership pass's extension-method path did not recognize the
// extension-block-level type parameter (`extension IEnumerable<T>`'s `T` rides
// the target type's generic-arg list, and `checkMember` was called with no
// enclosing context), so `checkBareTypeParamReturn` never fired for extension
// methods; and (b) even with the context recognized, the bare-identifier arm
// admitted any unseeded borrow, including an iteration variable whose source is
// the loop's implicit enumerator.
//
// Per #2064 (§28.3 v0.194, §3.6 rule 2), the body-side reject is decidable at
// the declaration only when ALL instantiations of the returned parameter are
// class/interface — a `where T: class` constraint at the block level forces
// this. For a mixed-domain parameter (no class constraint) the body is ADMITTED
// at the declaration and the per-call-site class-substitution check is the
// recorded residual; for a `where T: value` parameter the return copies the
// value and the admission is vacuous.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"        // runAot — AOT oracle for the accept pins
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The v0.235 iteration-variable reject message (a stable substring).
constexpr std::string_view kIterVarReject =
    "iteration variable borrowed from an interface-typed enumeration source";

}  // namespace

// ---------------------------------------------------------------------------
// MUST-REJECT (the bug). `extension IEnumerable<T> where T: class` whose body
// returns the iteration variable of a `for var item in this` loop. The loop
// source is statically `IEnumerable<T>` (an interface-typed enumeration), so the
// element's backing is unprovable; the iteration variable's source is the loop's
// implicit enumerator — a scope-bound local — and returning it hands back a
// borrow that dies with the callee frame while the caller re-establishes only a
// borrow. The `where T: class` constraint makes the reject decidable at the
// declaration (#2064 per-instantiation-category gate). This is the pre-v0.235
// §26.5.1 First() shape.
// FAILS WITHOUT THE FIX: the extension-block-level `T` was not recognized as a
// bare type parameter (it rides the target type's generic-arg list, and the
// ownership pass called checkMember with no enclosing context), so the body
// compiled clean — the unsound accept the ruling closes.
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           ienumerable_for_in_return_item_rejected_spec_28_3_v0_235) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: Exception "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "extension IEnumerable<T> where T: class "
        "  public method First(): T "
        "    for var item in this do return item; end; "
        "    throw new C(); "
        "  end First; "
        "end;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kIterVarReject));
}

// The `=>` expression-body sibling of the must-reject case would not apply here
// (a `for` loop is a statement form), so the block-body form above is the
// canonical pin. Below: the ACCEPT half of the ruling.

// ---------------------------------------------------------------------------
// ACCEPT (indexer spelling). `extension Array<T>` whose body returns `this[0]`
// — the index-based spelling on a receiver that declares an indexer is
// receiver-sourced and admissible (the ruling's accept half; §28.6's fields-
// and-indexers notion). The `where T: class` constraint turns the bare-T
// callee-side check ON, and the indexer read is correctly admitted (it is not
// an iteration variable).
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           array_indexer_return_accepted_spec_28_3_v0_235) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: Exception "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "extension Array<T> where T: class "
        "  public method FirstIdx(): T "
        "    return this[0]; "
        "  end FirstIdx; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ACCEPT (concrete collection-backed loop source). `extension Array<T>` whose
// body is `for var x in this do return x; end;`. The loop source is statically
// `Array<T>` — a CONCRETE collection — so the iteration variable's source chain
// runs element slot -> collection -> receiver (§29.1's re-borrow chain), and
// returning it is admissible. This is the issue's optional accept pin (#4); it
// guards against a false reject of the legitimate concrete-collection shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           array_for_in_return_item_accepted_spec_28_3_v0_235) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: Exception "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "extension Array<T> where T: class "
        "  public method FirstLoop(): T "
        "    for var x in this do return x; end; "
        "    throw new C(); "
        "  end FirstLoop; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ACCEPT (`where T: value` — the spec §26.5.1 form). The SAME for-in-return-item
// body as the MUST-REJECT case, but with `where T: value`: at every substitution
// T is a value type, so the return COPIES the iteration variable and the
// admission is vacuous (§28.3's `where T: value` exclusion, spec item 5). This
// is exactly the shape the post-ruling §26.5.1 First() example uses.
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           ienumerable_for_in_return_item_where_t_value_accepted_spec_26_5_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: Exception "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "extension IEnumerable<T> where T: value "
        "  public method First(): T "
        "    for var item in this do return item; end; "
        "    throw new C(); "
        "  end First; "
        "end;");
    BEZEL_EXPECT_FALSE(errorSays(r, kIterVarReject));
}

// ---------------------------------------------------------------------------
// ACCEPT (mixed-domain — the #2064 residual). The SAME for-in-return-item body
// as the MUST-REJECT case, but WITHOUT `where T: class` (no class constraint):
// at a value substitution the body is a value copy (vacuous), so the body-side
// reject is NOT decidable at the declaration — the per-call-site class-
// substitution check is the recorded residual (#2064). The declaration must be
// ADMITTED so legitimate mixed-domain helpers are not over-rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           ienumerable_for_in_return_item_mixed_domain_admitted_2064) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: Exception "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "extension IEnumerable<T> "
        "  public method First(): T "
        "    for var item in this do return item; end; "
        "    throw new C(); "
        "  end First; "
        "end;");
    BEZEL_EXPECT_FALSE(errorSays(r, kIterVarReject));
}

// ---------------------------------------------------------------------------
// ACCEPT (no-return loop — the legitimate iteration idiom). A `for var item in
// this` loop that does NOT return the iteration variable (it counts elements)
// must remain unaffected. Guards against a false reject of ordinary iteration.
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           ienumerable_for_in_count_accepted_spec_29_2) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: Exception "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "extension IEnumerable<T> where T: class "
        "  public method MyCount(): int "
        "    var n := 0; "
        "    for var item in this do n := n + 1; end; "
        "    return n; "
        "  end MyCount; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// AOT end-to-end (the indexer accept pin, value-verified). The `extension
// Array<T>` indexer-spelling `FirstIdx` runs at a class instantiation
// (`Array<C>`), returns `this[0]`, and the caller reads the element's member —
// a receiver-sourced borrow round-trip the ruling admits. Compiles AND runs to
// a clean exit, confirming the accept half is not a false admit.
// ---------------------------------------------------------------------------
BEZEL_TEST(iteration_variable_bare_t_2381,
           array_indexer_return_runs_at_class_substitution_spec_28_3) {
    std::string src =
        "module M; "
        "class C: Exception "
        "  public v: int := 0; "
        "  public constructor(n: int) v := n; end constructor; "
        "  destructor end destructor; "
        "  public method N(): int return this.v; end N; "
        "end C; "
        "extension Array<T> where T: class "
        "  public method FirstIdx(): T return this[0]; end FirstIdx; "
        "end; "
        "public static class P public static method Main(): int "
        "  var xs: Array<C> := new Array<C>(); "
        "  using var c := new C(42); "
        "  xs.Add(c); "
        "  var f := xs.FirstIdx(); "      // receiver-sourced borrow of this[0]
        "  return f.N() - 42; "           // = 42 - 42 = 0
        "end Main; end P;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
