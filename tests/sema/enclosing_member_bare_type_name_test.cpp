// fix §2.7 "Through nesting" (v0.338): a bare TYPE name's search ends at the
// first enclosing type that HAS THE NAME — a METHOD, PROPERTY, FIELD or STATIC
// of that name stops it exactly as a nested type does.
//
// §2.7, "Through nesting": "In a nested type's bodies (§11.6), the enclosing-
// type lookup visits the nested type and then each type enclosing it, innermost
// first, at the rank the table gives its members; THE FIRST TYPE THAT HAS AN
// ACCESSIBLE MEMBER OF THAT NAME ENDS THE SEARCH. … Ending the search at the
// first type that HAS THE NAME, EVEN WHEN THE MEMBER CANNOT BE USED THERE,
// keeps a bare name from passing over an enclosing member and silently
// resolving to a same-named module declaration or import."
//
// THE DEFECT. A type BODY SCOPE registers only the type's GENERIC PARAMETERS and
// NESTED TYPES (`resolveTypeDecl`'s §11.3/#482 loop) — never its fields,
// methods, properties or events. The scope chain IS §2.7's enclosing chain (a
// nested type's body scope parents to the enclosing type's body scope), so
// `lookupName` walked straight over every enclosing member of the name and
// reached a same-named type further out.
//
// MEASURED on the base compiler (`bezelc` built in the lane worktree at
// `c5930d59`, this branch's `merge-base` against staging `r30-c1`),
// `spec_2_7_nested_method_of_the_name_stops_a_bare_box_t` COMPILED CLEAN and
// bound `Box<int>` to `Outer`'s nested `Box<in X>`. Row 2 is that program with
// the `method Box` deleted, and it is the control in the direction that matters:
// the stop must be caused by the MEMBER, not by the spelling.
//
// THE ROWS. Every one is single-module and asserts one thing, and every ACCEPT
// row asserts `r.ran` so no skip-absorbing guard can stand in for the program.
//
//   * 1/2     — the pair above. DISCRIMINATOR: they differ only in whether the
//     enclosing type declares `method Box`.
//   * 3       — the OVER-REACH control: the same program with the nested `Box`
//     removed, so no same-named TYPE is reachable anywhere. The spelling is
//     unresolved either way; what the row pins is that the §2.7 clause is
//     present and says why, instead of a bare `unresolved type name` for a name
//     that IS declared one level out.
//   * 4/5/6   — PROPERTY, FIELD and STATIC of the name. §2.7 says "an accessible
//     MEMBER", not "method", and says the search ends "even when the member
//     cannot be used there" — a static member is admitted by bare name in a
//     VALUE position (§2.7) but a type position can use nothing at all.
//   * 7/8     — three levels, `Outer.Mid.Holder`, both `Mid` and `Holder`
//     declaring the name. §2.7's chain is "innermost first … the first type that
//     has … ends the search", so the diagnostic must name `Holder`. Row 8 drops
//     `Mid`'s member so that row 7 is about ORDER, not about two same-named
//     members merely existing.
//   * 9/10    — §2.7's `property Color: Color` CARVE-OUT, nested and top-level.
//     §2.7's warning row: "warn, UNLESS the member's declared type is the
//     resolved type (or an instantiation of the resolved generic) — the
//     `property Color: Color` idiom; qualify `this.` to reach the member". Row 9
//     is the NESTED position, where the stop rule otherwise applies; a member
//     that spells the colliding name in its own declared type cannot be a
//     blocker or it could not be declared. MEASURED: without the carve-out row 9
//     is rejected where the base accepts it. Row 10 is the top-level twin, which
//     `tests/codegen/choice_body_record_ctor_store_2097_test.cpp`
//     `spec_2_7_dotted_head_member_typed_as_colliding_type_accepted` already
//     pins upstream and which this fix must not regress.
//   * 11/12   — the TOP-LEVEL residual, pinned in both directions. §2.7's stop
//     sentence sits in a paragraph scoped to "a nested type's bodies", and
//     rule 1's normative table has NO row for a bare TYPE position — its rows
//     are a chain head, a bare value read, an assignment target, a bare callee, a
//     head continued by `?.`/`!` and the warning row, every one a position at
//     which a NAME is read. So a top-level type's OWN member does not end a bare
//     type-name search. Row 11 pins that; row 12 pins that the spelling resolves
//     there (so row 11's acceptance is a resolution, not a silent
//     non-diagnostic).
//   * 13/14/15— ACCESSIBILITY (§13.1), as a triple. A `private` member of an
//     OUTER enclosing type is not accessible from the nested body, and neither is
//     a `protected` one (a nested type does not derive from the type that
//     encloses it), so neither may end the search — rows 13/15. Row 14 is the
//     same member made `public`, which does. Together they say accessibility is
//     what separates them, not the member's mere existence.
//   * 16      — the `ownBody` half of that filter: a `private` member of the type
//     whose OWN body names it IS accessible there (§13.1 — `private` is
//     reachable within the declaring type), so it does end the search. Row 14's
//     `private` twin would be a false rejection.
//   * 17      — the RESOLVER/§12.9-CHECK COUPLING; see its own comment.
//   * 20–33   — §21.10 LOCAL TYPES (fix #6401): the local type as the
//     innermost link, its member-kind cross product, the spelling axis, the
//     enclosing link, accessibility through it, and §21.10's own carve-out.
//
// A stop that fired without a member (row 2/12), fired on the wrong member kind
// (rows 4/5/6), fired on an outer type before an inner one (rows 7/8), fired on
// an inaccessible member (13/15), failed to fire on an accessible one (14/16),
// fired inside the carve-out (9/10), or reached past a nested body (11/12) is
// caught by exactly one of these.
//
// NOT PINNED, AND DELIBERATELY SO — the stop SOURCES the implementation still
// declines. It is recorded as an issue rather than asserted here, because a row
// asserting the current behaviour would enshrine an acceptance §2.7 forbids and
// a row asserting the correct behaviour is red; it needs the ruling first.
//   * BASE TYPES — a member INHERITED from a base of an enclosing type does not
//     end the search (#6400, spec-question: rule 1's ranking paragraph says
//     "inherited included", the stop paragraph enumerates only enclosing links).
//
// §21.10 LOCAL TYPES were on this list until fix #6401, which gave a local type
// the body-scope pairing it lacked and made it the innermost link of §2.7's
// chain (rows 20–33 below).

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                           // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The §2.7 stop's own clause. It is APPENDED to the pre-existing
// `unresolved type name` message rather than replacing it, so a row that wants
// the plain form looks for the name alone.
const char* kUnresolved = "unresolved type name";
const char* kStopClause = "a bare name's search ends at the first enclosing type";
const char* kStopSection = "§2.7 Through nesting";

std::string program(const std::string& decls) {
    return "module M; import System; " + decls +
           " public class Program public static method Main() "
           "Console.WriteLine(\"x\"); end Main; end Program; ";
}

// `Outer`'s nested contravariant `Box`, and a nested class `Holder` whose
// `extra` declares the colliding name and whose field `M` is typed
// `Box<int>?`. `extra` empty is the control.
std::string nested(const std::string& extra) {
    return std::string("public class Outer ") +
           "public interface Box<in X> method Put(x: X); end Box; "
           "public class Holder " + extra +
           "private M: Box<int>? := null; end Holder; end Outer; ";
}

// A module-level `Box` (the §2.7 shape the harm sentence names: "a same-named
// module declaration"), an `Outer` declaring `extra` under that name, and
// `Outer.Mid.Holder` naming `Box` in type position.
std::string moduleLevel(const std::string& extra) {
    return std::string("public interface Box<in X> method Put(x: X); end Box; ") +
           "public class Outer " + extra +
           "public class Mid public class Holder "
           "private M: Box<int>? := null; end Holder; end Mid; end Outer; ";
}

// REJECT naming §2.7's stop rule. Asserts the rule's clause AND section mark, so
// a program rejected by the plain unresolved-name diagnostic alone cannot satisfy
// it, and asserts `r.ran` is false.
void expectStoppedAtEnclosingMember(const std::string& decls,
                                    const char* typeNamed) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kUnresolved));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStopClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStopSection));
    // §2.7's chain is innermost-first; the diagnostic must name the type that
    // ENDED the search, not merely one that has the name.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, typeNamed));
}

// ACCEPT: compiles and runs. `r.ran` is asserted.
void expectAccepted(const std::string& decls) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_TRUE(r.ran);
}

// --- §21.10 LOCAL-TYPE HELPERS (rows 20+) ---------------------------------
//
// `decls` is the body of a method that declares a local `record Inner`, gives
// it a member `Get` whose return type names `Box`, constructs it and reads the
// field back. `moduleBox` is §2.7's "same-named module declaration" — the harm
// the stop exists to prevent. `enclosing` is spliced into the TOP-LEVEL class
// that declares the method, so a row can put a member of the name on it.
std::string localInTopLevel(const std::string& decls,
                            const std::string& enclosing = "") {
    return "public record Box(V: int); public class Holder " + enclosing +
           " public method M(): int " + decls +
           " var i := new Inner(); return i.Get().V; "
           " end M; end Holder; ";
}

// The same, with the method declared by a NESTED class `Outer.Mid` and
// `enclosing` spliced into `Mid` — so the enclosing link of the chain is a
// nested type rather than a top-level one.
std::string localInNested(const std::string& enclosing,
                          const std::string& decls) {
    return "public record Box(V: int); public class Outer public class Mid " +
           enclosing + " public method M(): int " + decls +
           " var i := new Inner(); return i.Get().V; "
           " end M; end Mid; end Outer; ";
}

// A local `record Inner` whose `Get` names `Box`, carrying one member
// `memberDecls` under the name `Box`.
std::string localWithMember(const std::string& memberDecls) {
    return "record Inner " + memberDecls +
           " public method Get(): Box return new Box(7); end Get; "
           " end Inner; ";
}

}  // namespace

// --- 1/2. THE DEFECT, and its control -------------------------------------

// `Outer.Holder` declares `method Box(): int`; `Holder`'s body writes `Box<int>`,
// which must not bind to `Outer`'s nested `Box<in X>`. §2.7: the search ends at
// the first type that HAS the name, "even when the member cannot be used there".
// On the base this compiled clean and ran.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_nested_method_of_the_name_stops_a_bare_box_t) {
    expectStoppedAtEnclosingMember(
        nested("public method Box(): int return 1; end Box; "), "`Holder`");
}

// The control in the direction that matters: the SAME program with the
// `method Box` deleted. Nothing about the spelling changes, so a rejection here
// would say the stop is driven by the spelling rather than by the member.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_nested_control_without_the_member_still_resolves) {
    expectAccepted(nested(""));
}

// --- 3. THE OVER-REACH CONTROL -------------------------------------------
//
// The `method Box` stays and the nested `Box` goes, so no same-named TYPE is
// reachable anywhere. The spelling is unresolved either way — the row pins that
// the §2.7 clause is present, so the diagnostic explains that the search ended
// at `Holder` rather than leaving `Box` looking undeclared when it is declared
// one level out.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_member_without_any_nested_type_names_the_stop_reason) {
    expectStoppedAtEnclosingMember(
        "public class Outer public class Holder "
        "public method Box(): int return 1; end Box; "
        "private M: Box<int>? := null; end Holder; end Outer; ",
        "`Holder`");
}

// --- 4/5/6. PROPERTY, FIELD and STATIC ------------------------------------

// §2.7 says "an accessible MEMBER of that name", not "method".
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_nested_property_of_the_name_stops_a_bare_box_t) {
    expectStoppedAtEnclosingMember(
        nested("public readonly property Box: int := 0; "), "`Holder`");
}

BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_nested_field_of_the_name_stops_a_bare_box_t) {
    expectStoppedAtEnclosingMember(nested("private Box: int := 0; "),
                                   "`Holder`");
}

// A STATIC member is admitted by bare name in a VALUE position (§2.7 admits "a
// static member or a nested type" of an enclosing type), but a TYPE position
// can use nothing at all, so the search still ends. Without this row a fix that
// skipped `static` members would pass every other row here.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_nested_static_method_of_the_name_stops_a_bare_box_t) {
    expectStoppedAtEnclosingMember(
        nested("public static method Box(): int return 1; end Box; "),
        "`Holder`");
}

// --- 7/8. THREE LEVELS, innermost first ----------------------------------
//
// `Outer` carries the nested `Box`, and both `Mid` and `Holder` declare a member
// of that name. §2.7: "innermost first … the first type that has an accessible
// member of that name ends the search" — so `Holder` ends it, and a walk that
// visited `Mid` first would name `Mid` in the diagnostic.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_three_levels_the_innermost_member_ends_the_search) {
    expectStoppedAtEnclosingMember(
        "public class Outer public interface Box<in X> method Put(x: X); "
        "end Box; public class Mid public method Box(): int return 1; "
        "end Box; public class Holder public method Box(): string return \"\"; "
        "end Box; private M: Box<int>? := null; end Holder; end Mid; "
        "end Outer; ",
        "`Holder`");
}

// Row 8 drops `Mid`'s member, so row 7 is about ORDER rather than about two
// same-named members merely existing.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_three_levels_holder_alone_still_ends_the_search) {
    expectStoppedAtEnclosingMember(
        "public class Outer public interface Box<in X> method Put(x: X); "
        "end Box; public class Mid public class Holder "
        "public method Box(): string return \"\"; end Box; "
        "private M: Box<int>? := null; end Holder; end Mid; end Outer; ",
        "`Holder`");
}

// --- 9/10. §2.7's `property Color: Color` CARVE-OUT ----------------------

// NESTED, which is where the stop rule applies. §2.7's warning row: "warn,
// unless the member's declared type is the resolved type (or an instantiation of
// the resolved generic) — the `property Color: Color` idiom". `Inner`'s
// constructor names the module-level `choice Color`, and `Outer`'s `property
// Color: Color` cannot be a blocker — or `Inner` could not name the type at all.
// MEASURED: without the carve-out this program is rejected where the base
// accepts it.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_property_color_color_carve_out_in_a_nested_body) {
    expectAccepted(
        "public choice Color Red, Green(value: int); end Color; "
        "public class Outer public property Color: Color := Color.Red; "
        "public record Inner public constructor(c: Color) Color := c; "
        "end constructor; end Inner; end Outer; ");
}

// Top-level, the boundary of the whole fix — the same idiom one level out. It
// is pinned upstream by
// `codegen_choice_body_record_ctor_store_2097.spec_2_7_dotted_head_member_typed_as_colliding_type_accepted`,
// which this change must not regress.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_property_color_color_carve_out_in_a_top_level_body) {
    expectAccepted(
        "public choice Color Red, Green(value: int); end Color; "
        "public record Wrap public property Color: Color := Color.Red; "
        "public constructor(c: Color) Color := c; end constructor; "
        "public method IsRed(): bool => Color = Color.Red; end Wrap; ");
}

// --- 11/12. THE TOP-LEVEL RESIDUAL, pinned in both directions -----------

// A top-level type's OWN member does not end a bare type-name search: §2.7's
// stop sentence is scoped to "a nested type's bodies", and rule 1's normative
// table has no row for a type position.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_top_level_own_member_does_not_end_a_bare_type_search) {
    expectAccepted("public interface Box<in X> method Put(x: X); end Box; "
                   "public class Holder public method Box(): int return 1; "
                   "end Box; private M: Box<int>? := null; end Holder; ");
}

// Row 12: the same program with the `method Box` deleted, so row 11's acceptance
// is the RESOLUTION rather than a silent non-diagnostic.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_top_level_control_without_the_member_is_unresolved) {
    expectAccepted("public interface Box<in X> method Put(x: X); end Box; "
                   "public class Holder private M: Box<int>? := null; "
                   "end Holder; ");
}

// --- 13/14/17. ACCESSIBILITY (§13.2), as a triple ------------------------

// `private` on an OUTER enclosing type is not reachable from the nested body
// (§13.2), so it must not end the search; the bare name still reaches the
// module-level `Box`. Rejecting here would be an over-rejection with no basis.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_private_outer_member_is_not_accessible_so_does_not_stop) {
    expectAccepted(moduleLevel(
        "private method Box(): int return 1; end Box; "));
}

// Row 14 is row 13's `public` twin: the same member made accessible DOES end the
// search, which is what makes the pair about accessibility rather than about
// whether the compiler can see the member at all.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_public_outer_member_is_accessible_so_it_does_stop) {
    expectStoppedAtEnclosingMember(
        moduleLevel("public method Box(): int return 1; end Box; "), "`Outer`");
}

// `protected` is the second half of the same rule: reachable within the
// declaring type and types DERIVING from it, and a nested type does not derive
// from the type that encloses it.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_protected_outer_member_is_not_accessible_either) {
    expectAccepted(moduleLevel(
        "protected method Box(): int return 1; end Box; "));
}

// --- 18. THE `ownBody` HALF OF THAT FILTER -------------------------------
//
// A `private` member of the type whose OWN body names it is reachable there
// (§13.1 — `private` is visible within the declaring type), so it does end the
// search. Filtering `private` unconditionally would reject this and would be
// wrong.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_private_member_of_the_nested_type_itself_does_stop) {
    expectStoppedAtEnclosingMember(
        nested("private method Box(): int return 1; end Box; "), "`Holder`");
}

// --- 19. THE RESOLVER/§12.9-CHECK COUPLING -------------------------------
//
// #6383's `enclosingMemberBareName` (`src/sema/TypeResolution_Checks.cpp`,
// commit `60cc1e08`, reviewed ACCEPT-W-NOTES, NOT yet picked) answers the §2.7
// bare-name question for §12.9 from the SAME enclosing chain — and it walks that
// chain reading only `NestedTypeSymbol`, so it passes over a METHOD or PROPERTY
// of the name exactly as this resolver did. Its own comment says so: "This
// function therefore matches the resolver rather than §2.7, and MUST be changed
// with it: when the resolver is fixed to end the search, this must end it too,
// or the two will disagree."
//
// Row 19 pins the RESOLVER's answer on exactly the spelling that arm reads, so
// the halves cannot be changed apart silently. The shape is #6383's: an
// enclosing CLASS carrying a nested contravariant `Box<in X>`, and a nested
// `Holder<out T>` naming it by BARE NAME in a return slot — with `Holder` ALSO
// declaring `method Box`, which is what makes the two halves differ.
//
// MEASURED on a COMPOSED tree (this branch's `TypeResolution_Names.cpp` plus
// #6383's `60cc1e08`, built out of tree at
// `/var/tmp/bezel-scratch/r31-p-27nest/composed`):
//
//   * no same-named member   → the resolver BINDS `Box<T>` to `Outer`'s nested
//     `Box<in X>`, `enclosingMemberBareName` composes from it, §12.9 REPORTS.
//     The two agree.
//   * WITH `method Box`      → the resolver now REJECTS naming §2.7 (this row),
//     but `enclosingMemberBareName` walks past the method and STILL composes, so
//     the program carries BOTH the §2.7 stop and a §12.9 line. That is the
//     divergence #6383's comment leaves to the coordinator. It is an ADDED line
//     on an already-rejected program: no program that used to compile stops
//     compiling, and none that used to be rejected becomes accepted.
//   * the `property X: X` carve-out → the resolver keeps the name BOUND, and the
//     composed tree's §12.9 answer is unchanged from its own base.
//
// This row asserts the RESOLVER half only, deliberately. §12.9's half is
// #6383's to own, and any row asserting the §12.9 diagnostic's presence or
// absence on these shapes would be RED on this branch (the arm is absent) and
// would flip again when it lands. Pinning a fact that is not yet true on the
// branch carrying the row is the defect, not the fix.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_agreement_with_6383_the_resolver_stops_at_the_member) {
    expectStoppedAtEnclosingMember(
        "public class Outer public interface Box<in X> method Put(x: X); "
        "end Box; public interface Holder<out T> method Box(): int; "
        "method Get(): Box<T>; end Holder; end Outer; ",
        "`Holder`");
}
// ===========================================================================
// §21.10 LOCAL TYPES (fix #6401) — the local type is a LINK of the chain
// ===========================================================================
//
// §21.10, "Scope and name discipline": "Inside the local type's OWN MEMBER
// BODIES, a bare name reaches the static members and nested types of the type
// whose body declares it, and of that type's enclosing types, by §2.7's
// Through nesting." §2.7's own scope sentence names the same chain shape —
// "the enclosing-type lookup visits the nested type and then each type
// enclosing it, innermost first … THE FIRST TYPE THAT HAS AN ACCESSIBLE MEMBER
// OF THAT NAME ENDS THE SEARCH" — and a §21.10 local type is body-local, so
// §2.7's chain for one of its member bodies is `Inner`, then the type whose
// body declares it, then that type's enclosing types.
//
// THE DEFECT (#6401). A local type got NO body scope pairing at all: the walk
// over `typeBodyScopes_` reaches a module-level or nested type because
// `resolveTypeDecl` pairs that type's body scope with its TypeSymbol, and a
// §21.10 local type is never handed to `resolveTypeDecl` — so it was not a
// LINK of the chain, and a bare TYPE name in one of its member bodies sailed
// past its OWN members and reached the same-named module declaration. §2.7's
// harm sentence calls that outcome out by name.
//
// MEASURED on the base (`bezelc` built in the lane worktree at da1ec4d5), row
// 20 COMPILED CLEAN AND RAN, binding `Box` to the module-level `record Box`.
// Row 21 is that program with the member deleted — the control in the
// direction that matters: the rejection must be caused by the MEMBER, not by
// the spelling.

// --- 20/21. THE DEFECT, and its control -----------------------------------

BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_types_own_member_stops_a_bare_type_search) {
    expectStoppedAtEnclosingMember(
        localInTopLevel(localWithMember(
            "public method Box(): int return 1; end Box; ")),
        "`Inner`");
}

BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_type_without_the_member_still_resolves) {
    expectAccepted(localInTopLevel(localWithMember("")));
}

// --- 22/23/24/25. THE MEMBER-KIND CROSS PRODUCT ---------------------------
//
// §2.7 says "an ACCESSIBLE MEMBER of that name" and "even when the member
// CANNOT BE USED THERE"; it does not say "method". A fix that read only method
// symbols would satisfy rows 20/21 and every one of these would stay green
// while the defect stood for the other member kinds.

BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_type_property_of_the_name_stops_the_search) {
    expectStoppedAtEnclosingMember(
        localInTopLevel(localWithMember(
            "public property Box: int := 3; ")),
        "`Inner`");
}

// A `private` member of the type whose OWN body names it IS reachable there
// (§13.1 — `private` is visible within the declaring type), so it stops the
// search. Filtering `private` unconditionally would reject this.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_types_own_private_field_stops_the_search) {
    expectStoppedAtEnclosingMember(
        localInTopLevel(localWithMember("private Box: int := 3; ")),
        "`Inner`");
}

// A STATIC member is admitted by bare name in a VALUE position (§2.7 admits "a
// static member or a nested type" of an enclosing type), but a TYPE position
// can use nothing at all, so the search still ends.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_type_static_member_of_the_name_stops_the_search) {
    expectStoppedAtEnclosingMember(
        localInTopLevel(localWithMember(
            "public static readonly Box: int := 3; ")),
        "`Inner`");
}

BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_type_const_of_the_name_stops_the_search) {
    expectStoppedAtEnclosingMember(
        localInTopLevel(localWithMember("public const Box: int := 3; ")),
        "`Inner`");
}

// --- 26. THE POSITIONAL-FIELD SPELLING ------------------------------------
//
// `record Inner(Box: int)` puts the colliding name on the record's PRIMARY
// field surface rather than on a member declaration. A fix that walked only
// `decl->members` would miss it; this row says the walk reaches it.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_type_positional_field_of_the_name_stops_the_search) {
    expectStoppedAtEnclosingMember(
        "public record Box(V: int); public class Holder public method M(): "
        "int record Inner(Box: int) public method Get(): Box return new "
        "Box(7); end Get; end Inner; var i := new Inner(7); "
        "return i.Get().V; end M; end Holder; ",
        "`Inner`");
}

// --- 27. THE SPELLING AXIS: a QUALIFIED head is a DIFFERENT ROW ------------
//
// The stop is scoped to a BARE name. `Outer.Box` is a chain head, which is a
// different row of §2.7's normative table where the enclosing type's members
// rank "last, after aliases"; stopping there would invert that row. So the
// same local type, with the same `method Box` of the name, still resolves the
// QUALIFIED spelling — the discriminator between rows 20 and 27.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_qualified_head_in_a_local_type_still_resolves) {
    expectAccepted(
        "public class Outer public record Box(W: int); public method M(): int "
        "record Inner public method Box(): int return 1; end Box; "
        "public method Get(): Outer.Box return new Outer.Box(7); end Get; "
        "end Inner; var i := new Inner(); return i.Get().W; "
        "end M; end Outer; ");
}

// --- 28/29. THE ENCLOSING LINK OF A LOCAL TYPE'S CHAIN --------------------
//
// §21.10's sentence names "the type whose body declares it, and of that type's
// enclosing types", so the chain does not stop at the local type. §2.7's chain
// for the body we are in reaches the declaring type, and a member of the name
// there ends the search — whether the declaring type is top-level or nested
// (row 30/31 below is the nested twin, and row 14 already pins a top-level
// enclosing type stopping for a NESTED body's chain).
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_enclosing_toplevel_member_stops_a_local_types_search) {
    expectStoppedAtEnclosingMember(
        localInTopLevel(localWithMember(""),
                        "public method Box(): int return 1; end Box; "),
        "`Holder`");
}

// --- 30/31. ACCESSIBILITY THROUGH A LOCAL TYPE ----------------------------
//
// §2.7 (v0.338), local-type sentence, in its own words: "Accessibility there is
// that of the body declaring the local type, so the `private` members of the
// type declaring it count as accessible." So a `private`/`protected` member of
// the DECLARING type is an accessible member of the chain inside the local
// type's body, and §2.7's stop sentence — "the first type that has an
// accessible member of that name ends the search" — ends there.
//
// These rows are GREEN on the base and RED with the §13.1-based reading: with a
// nested-type-style reachability filter applied inside a local type's body, the
// declaring type's `private`/`protected` members are filtered away, the search
// walks past them and lands on the module-level `record Box` — the SILENT
// ACCEPTANCE §2.7's harm sentence names ("keeps a bare name from passing over an
// enclosing member and silently resolving to a same-named module declaration or
// import"). MEASURED both ways on this lane: base REJECTED naming `Mid`; the
// §13.1-based form CLEAN-COMPILED and bound the module record.
//
// They are therefore a GUARD on the ADD half, not part of its red-without set —
// the eight rows that prove the fix are 20, 22, 23, 24, 25, 26, 28, 29, 33.
//
// ⚠ ROWS 30/31 WERE, IN AN EARLIER FORM, `expectAccepted` ROWS asserting the
// opposite. That assertion rested on §13.1's plain reachability rule read into
// a body §2.7 explicitly gives the DECLARING body's accessibility, and it
// enshrined a silent acceptance; review measured it (base REJECTED, that draft
// CLEAN-COMPILED and bound the module record) and it is gone.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_enclosing_private_member_stops_inside_a_local_type) {
    expectStoppedAtEnclosingMember(
        localInNested("private method Box(): int return 1; end Box; ",
                      localWithMember("")),
        "`Mid`");
}

BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_enclosing_protected_member_stops_inside_a_local_type) {
    expectStoppedAtEnclosingMember(
        localInNested("protected method Box(): int return 1; end Box; ",
                      localWithMember("")),
        "`Mid`");
}

// The `public` twin of rows 30/31 on the same spelling, so the pair is about
// ACCESSIBILITY and not about whether the compiler can see the member at all.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_enclosing_public_member_still_stops_inside_a_local_type) {
    expectStoppedAtEnclosingMember(
        localInNested("public method Box(): int return 1; end Box; ",
                      localWithMember("")),
        "`Mid`");
}

// --- 32. §21.10's OWN carve-out, which this must not break ----------------
//
// §21.10: "The prohibition does NOT reach the local type's OWN members: …
// `public method Twin(): Pair` and `public method Merge(other: Pair): Pair`
// inside a local `record Pair` are therefore admitted." A stop that fired on a
// member naming the LOCAL TYPE ITSELF would make that carve-out unspellable —
// `Twin(): Pair` names `Pair` in a bare type position inside `Pair`. So the
// name of the local type is not a member of it and the search is unaffected.
// This is row 20 with the colliding name changed to the local type's own.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_local_types_own_members_may_name_the_local_type) {
    expectAccepted(
        "public class Holder public method M(): int "
        "record Pair(A: int, B: int) "
        "  public method Twin(): Pair return Pair(1, 2); end Twin; "
        "  public method Merge(other: Pair): Pair "
        "    return Pair(other.A + 1, other.B + 1); "
        "  end Merge; "
        "end Pair; "
        "var p := new Pair(1, 2); return p.Merge(p.Twin()).A; "
        "end M; end Holder; ");
}

// --- 33. A LOCAL TYPE INSIDE A LOCAL TYPE (save/restore reachability) -----
//
// The innermost-body marker is set on entering a local type's members and
// restored on the way out, so a local type declared inside a METHOD of another
// local type must leave the outer one's link behind: `Deep`'s body resolves
// `Box` against the chain `Deep`, `Inner`, `Holder`, and stops at `Inner`,
// which is the first of those with a member of the name. A save/restore that
// leaked would either stop at `Deep` (which has none) and bind the module
// `Box`, or miss the `Inner` link entirely.
BEZEL_TEST(enclosing_member_bare_type_name,
           spec_2_7_a_local_type_inside_a_local_type_still_sees_the_outer_link) {
    expectStoppedAtEnclosingMember(
        "public record Box(V: int); public class Holder public method M(): "
        "int record Inner "
        "  public method Box(): int return 1; end Box; "
        "  public method M2(): int "
        "    record Deep public V: Box := new Box(7); end Deep; "
        "    var d := new Deep(); return d.V.V; "
        "  end M2; "
        "end Inner; "
        "var i := new Inner(); return i.M2(); "
        "end M; end Holder; ",
        "`Inner`");
}
