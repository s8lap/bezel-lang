// fix #7115 (§14.1 / §3.6 rule 2, v0.343): the base-chain substitution binds
// a base's type argument by NAME only, so a CONSTRUCTED type argument stays
// unsubstituted and the inherited member keeps the DERIVED class's parameter.
//
// THE SPEC.
//
//   * §14.1 *Instantiation and its site* (v0.343): "An **instantiation** of a
//     generic declaration is the constructed type or constructed member
//     obtained by **substituting type arguments for its type parameters**; its
//     **site** is the program point at which the program forms one." A base
//     clause naming a generic base IS such a site.
//   * §3.6 rule 2 (v0.343) *Per-instantiation evaluation for generics*:
//     "For a generic type or member, the property is
//     evaluated at each instantiation, **after substituting the type
//     arguments**."
//
// So in `class Mid<U>: Root<Array<U>>`, the base clause is an instantiation
// site of `Root`, and at `Mid<int>` that instantiation is `Root<Array<int>>`:
// the INHERITED indexer's parameter type is `Array<int>` there — NOT
// `Array<U>`.
//
// WHAT THE PRE-FIX COMPILER DID. The base-chain walk
// (`ExprTypecheck_Calls.cpp`, `ancestorGenericSubst`) looked a BARE
// single-part name up in the current level's bindings and left every other
// shape — a CONSTRUCTED argument above all — untouched. `Array<U>` therefore
// stayed `Array<U>`, with `U` still the DERIVED class's parameter. MEASURED on
// the pre-fix base (e8116142):
//
//   * `m["not an array"]` where `m: Mid<int>` (the INHERITED indexer) —
//     ACCEPTED: the §5.1 index-argument question was undecidable (the
//     declared type still carried an unsubstituted parameter), so #7064's
//     check failed OPEN and the wrong-typed argument reached the call.
//   * `r["not an array"]` where `r: Root<Array<int>>` (the DIRECT
//     instantiation, the SAME declaration) — REJECTED with §5.1's own
//     wording.
//
// The SAME declaration, reached two ways, got TWO different verdicts on the
// SAME argument type. The direct case is what the spec requires, so the
// inherited case must be rejected too.
//
// It was worse than a missing diagnostic. `m[1.5]` reached codegen with a
// `double` where the accessor wanted an `Array<int>`, and the LLVM verifier
// rejected the module ("Call parameter type does not match function
// signature!", "IR verification failed (malformed IR emitted)") — an internal
// compiler error where §5.1 asks for a clean diagnostic.
//
// It also REJECTED A VALID PROGRAM. An inherited METHOD whose parameter
// embeds the base's type argument (`method Take(x: T)` on `Root<T>`,
// reached through `Mid<U>: Root<Array<U>>`) was checked against the
// unsubstituted `Array<U>`, and the valid `m.Take(a)` with `a: Array<int>`
// was rejected.
//
// The rows below pin the repaired verdicts. Every rejection row asserts §5.1's
// WORDING as well as the rejection: `failedAtCompileTimeWithRealLocation`
// alone is satisfied by a crash or an IR-verification failure — which is
// exactly what the pre-fix `m[1.5]` produced.
//
// All rows are on the `runAot` oracle (the in-process JIT harness is
// deprecated).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §5.1's wording for a rejected conversion, pinned as a whole so a reworded
// diagnostic is a visible change rather than a silent one.
const char* kNotConvertible =
    "has a type that is not implicitly convertible to the target type (§5.1)";

// The rejection oracle: the rejection AND the wording. `r.ran == false` alone
// is satisfied by a crash, a signal, a timeout, an ASan report and an
// IR-verification failure alike, so it discriminates nothing on its own.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// The declarations under test: a generic base whose indexer's parameter is
// the type parameter, and a derived class that fixes it to a CONSTRUCTED type
// carrying the derived class's OWN parameter.
std::string decls() {
    return "public class Root<T> "
           "  public constructor() end constructor; "
           "  public virtual property [i: T]: string => \"root\"; "
           "end Root; "
           "public class Mid<U>: Root<Array<U>> "
           "  public constructor() end constructor; "
           "end Mid; ";
}

std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "public class Program public static method Main(): int\n" + mainBody +
           "end Main; end Program;\n";
}

}  // namespace

// ── the inherited indexer: the wrong-typed argument is now rejected ───────

// The defect's own repro, on the INHERITED side: at `Mid<int>` the inherited
// indexer's parameter type is `Array<int>` (§14.1 / §3.6 rule 2), and a
// `string` has no §5.1 row to `Array<int>` — so the argument is rejected.
// Pre-fix this was ACCEPTED (the fail-open), because the declared type still
// carried the unsubstituted `U`.
BEZEL_TEST(ancestor_generic_subst_7115,
           inherited_indexer_rejects_a_non_convertible_index_spec_14_2) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  if m[\"not an array\"] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         decls()));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `Mid[i]`"));
}

// The DIRECT instantiation of the SAME declaration, read the other way round:
// it was already rejected before this fix, and it must stay rejected with the
// SAME diagnostic. This is the asymmetry the issue names — the two arms of it
// must now agree.
BEZEL_TEST(ancestor_generic_subst_7115,
           direct_instantiation_rejects_the_same_argument_spec_14_2) {
    auto r = runAot(prog("  using var r := new Root<Array<int>>();\n"
                         "  if r[\"not an array\"] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         "public class Root<T> "
                         "  public constructor() end constructor; "
                         "  public virtual property [i: T]: string => \"root\"; "
                         "end Root; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `Root[i]`"));
}

// The ACCEPT control on the inherited side: a matching argument still
// compiles, links, runs and exits 1. This is the row #7064's fail-open was
// written for — narrowing that fail-open must not close this one.
BEZEL_TEST(ancestor_generic_subst_7115,
           inherited_indexer_accepts_a_matching_index_spec_14_2) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  const a: Array<int> := [1, 2];\n"
                         "  if m[a] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         decls()));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotConvertible));
}

// A `double` index is a §5.1 error, and it is now a DIAGNOSTIC: pre-fix it
// reached codegen and the LLVM verifier rejected the module ("IR verification
// failed (malformed IR emitted)"). The wording assertion is what makes this
// row discriminate — `failedAtCompileTimeWithRealLocation` alone is satisfied
// by the verifier failure too.
BEZEL_TEST(ancestor_generic_subst_7115,
           inherited_indexer_rejects_a_double_index_without_an_ice_spec_5_1) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  if m[1.5] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         decls()));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// An argument of the right SHAPE but the wrong element type is rejected too:
// `Array<string>` has no §5.1 row to `Array<int>`.
BEZEL_TEST(ancestor_generic_subst_7115,
           inherited_indexer_rejects_a_wrong_element_type_spec_5_1) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  const a: Array<string> := [\"x\"];\n"
                         "  if m[a] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         decls()));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// ── the inherited METHOD: a VALID program was rejected before the fix ────

// The issue's other measured consequence: a method whose parameter embeds the
// base's type argument was checked against the UNSUBSTITUTED `Array<U>`, so
// the valid call was rejected. At `Mid<int>` the parameter type is
// `Array<int>` and the call is admitted (§3.6 rule 2).
BEZEL_TEST(ancestor_generic_subst_7115,
           inherited_method_accepts_the_substituted_parameter_spec_14_2) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  const a: Array<int> := [1, 2];\n"
                         "  if m.Take(a) = 1 then return 1; end;\n"
                         "  return 0;\n",
                         "public class Root<T> "
                         "  public constructor() end constructor; "
                         "  public virtual method Take(x: T): int => 1; "
                         "end Root; "
                         "public class Mid<U>: Root<Array<U>> "
                         "  public constructor() end constructor; "
                         "end Mid; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotConvertible));
}

// And the same method with a wrong-typed argument is rejected — the §5.1
// question is now DECIDABLE at that position, where before it was not.
BEZEL_TEST(ancestor_generic_subst_7115,
           inherited_method_rejects_a_non_convertible_argument_spec_5_1) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  if m.Take(\"not an array\") = 1 then return 1; end;\n"
                         "  return 0;\n",
                         "public class Root<T> "
                         "  public constructor() end constructor; "
                         "  public virtual method Take(x: T): int => 1; "
                         "end Root; "
                         "public class Mid<U>: Root<Array<U>> "
                         "  public constructor() end constructor; "
                         "end Mid; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// ── the shapes that must NOT change ───────────────────────────────────────

// A BARE type parameter in the base clause (`Root<U>`) is the shape that
// already worked — the pre-fix walk's bare-name lookup handled it. It must
// keep working, and the §5.1 question at the index position must keep its
// verdict: `int` is admitted, a `string` is not.
BEZEL_TEST(ancestor_generic_subst_7115,
           bare_type_parameter_in_the_base_clause_is_unchanged_spec_14_2) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  if m[1] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         "public class Root<T> "
                         "  public constructor() end constructor; "
                         "  public virtual property [i: T]: string => \"root\"; "
                         "end Root; "
                         "public class Mid<U>: Root<U> "
                         "  public constructor() end constructor; "
                         "end Mid; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(ancestor_generic_subst_7115,
           bare_type_parameter_in_the_base_clause_still_rejects_spec_5_1) {
    auto r = runAot(prog("  using var m := new Mid<int>();\n"
                         "  if m[\"not an int\"] = \"root\" then return 1; end;\n"
                         "  return 0;\n",
                         "public class Root<T> "
                         "  public constructor() end constructor; "
                         "  public virtual property [i: T]: string => \"root\"; "
                         "end Root; "
                         "public class Mid<U>: Root<U> "
                         "  public constructor() end constructor; "
                         "end Mid; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// The TWO-LINK chain — the shape #6319's
// `indexer_vtable_dispatch_6319.ancestor_chain_substitutes_generic_key`
// uses: `Leaf<V>: Mid<V>` over `Mid<U>: Root<Array<U>>`. The substitution
// composes through both links (`V := int`, then `U := int`, then
// `T := Array<int>`) and the inherited indexer is admitted.
BEZEL_TEST(ancestor_generic_subst_7115,
           two_link_chain_composes_the_substitution_spec_14_2) {
    auto r = runAot(prog("  using var l := new Leaf<int>();\n"
                         "  const a: Array<int> := [1, 2];\n"
                         "  var m: Mid<int> := l;\n"
                         "  if m[a] = \"leaf\" then return 1; end;\n"
                         "  return 0;\n",
                         "public class Root<T> "
                         "  public constructor() end constructor; "
                         "  public virtual property [i: T]: string => \"root\"; "
                         "end Root; "
                         "public class Mid<U>: Root<Array<U>> "
                         "  public constructor() end constructor; "
                         "end Mid; "
                         "public class Leaf<V>: Mid<V> "
                         "  public constructor() end constructor; "
                         "  public override property [i: Array<V>]: string "
                         "    => \"leaf\"; "
                         "end Leaf; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotConvertible));
}

// The THREE-LINK chain: `C<V>: B<V>` over `B<U>: A<Array<U>>` over `A<T>`.
// The constructed argument sits two levels down from the receiver.
BEZEL_TEST(ancestor_generic_subst_7115,
           three_link_chain_composes_the_substitution_spec_14_2) {
    auto r = runAot(prog("  using var c := new C<int>();\n"
                         "  const a: Array<int> := [1, 2];\n"
                         "  if c[a] = \"a\" then return 1; end;\n"
                         "  return 0;\n",
                         "public class A<T> "
                         "  public constructor() end constructor; "
                         "  public virtual property [i: T]: string => \"a\"; "
                         "end A; "
                         "public class B<U>: A<Array<U>> "
                         "  public constructor() end constructor; "
                         "end B; "
                         "public class C<V>: B<V> "
                         "  public constructor() end constructor; "
                         "end C; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotConvertible));
}
