// §28.1 / §35.1 — the `owned` CHOICE-PAYLOAD marker's type requirement.
//
// §28.1 "Type requirement" (v0.334) enumerates the positions where `owned`
// carries a type requirement and names choice payloads among them:
//
//   "In the method-parameter, return-type, `out`-parameter, lambda-parameter,
//    choice-payload (§35.1), and function-type parameter/return slot positions
//    above, `owned` requires the declared type to be a class type other than
//    `Type`, an interface type, or a bare type parameter."
//
// and clause 2 makes every other type a compile error:
//
//   "Any other type — a primitive, `string`, `Type`, a function type, an
//    ordinary record, an enum, a non-owning inline array, or the unit type:
//    `owned` is a **compile error**."
//
// §35.1 (v0.334) carries it to the payload position verbatim:
//
//   "The `owned` payload marker's admissibility follows §28.1's type
//    requirement verbatim: admissible on class types, interface types, and bare
//    type parameters; admitted-but-redundant on move-only value types; a
//    compile error on any other type."
//
// THE DEFECT: `Resolver::resolveTypeDecl` resolved each case payload's type
// (TypeResolution_Names.cpp, "choice cases' payload fields") but never ran the
// payload through `checkOwnedDeclType` — the function that implements §28.1's
// type requirement for every OTHER admitted position (method parameters,
// returns, `out` parameters, operator returns, function-type slots, lambda
// parameters, and, since #4685, class `owned` fields). So a choice payload
// carrying `owned` on a primitive compiled clean with no diagnostic at all:
// a silent acceptance of a program §28.1 clause 2 requires rejected.
//
// This is the payload-side twin of the #4685 field-side gap; see the field
// branch in `Resolver::resolveMember` for the precedent this mirrors. Unlike
// that branch there is NO §28.5 double-marking carve-out to make room for: a
// non-owning inline-array payload rejects and an owning one is
// admitted-but-redundant by §28.1's own move-only clause — which is exactly
// what `checkOwnedDeclType`'s InlineArray arm already implements, so the
// payload position defers to that machinery wholesale.
//
// CORPUS ROW: tests/conformance/rules/cases/s35_1_0006.bzl pins the primitive
// shape (`Ready(owned code: int);`). Its `diagnostic:` needle is `owned`.
//
// Per CLAUDE.md's "admit controls" discipline, each REJECT row below is paired
// with the nearest-neighbour ADMIT row that pins the check's boundary — a
// control that cannot fail pins nothing, so each admits-controls row states
// what would break if the check were deleted or over-broad.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// fix #3985: the shared gated helper (sema_support.hpp) rejects a program
// whose parse itself failed, so a row here measures the TYPE check rather
// than accidentally pinning a syntax error.
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// §28.1's reject diagnostic, keyed on the invariant prefix so every
// inadmissible-type row shares one predicate. The message the fix renders is
// "`owned` is not admissible on <positionDesc> of type <desc> — …".
bool hasOwnedNotAdmissible(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`owned` is not admissible") != std::string::npos)
            return true;
    }
    return false;
}

// The same reject under its bare-type-parameter specialization, which
// `checkOwnedDeclType` emits directly instead of routing through the generic
// message (§28.1 clause 5: "rejects the marker at the declaration").
bool hasOwnedValueBoundReject(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("can never be a class or interface") !=
                std::string::npos)
            return true;
    }
    return false;
}

// A module-level shell with an accepted `Main` so no row's verdict can be an
// entry-point artifact: `parseAndTypecheckGated` reports the §33.1/§33.2
// "program has no entry point" error as a diagnostic, and a declaration-only
// program would then be rejected for the wrong reason (which is precisely how
// s35_1_0006's own verdict was masked).
constexpr std::string_view kMain =
    "class Program public static method Main() end Main; end Program; ";

}  // namespace

// ===========================================================================
// CORPUS ROW s35_1_0006 — `owned` on a PRIMITIVE payload (int).
// §28.1 clause 2 names "a primitive" first in its inadmissible list.
// ===========================================================================
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_primitive_payload_rejected_spec_28_1_s35_1_0006) {
    auto r = semaAll(
        "module Conf; "
        "choice Outcome Ready(owned code: int); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// The corpus case is a DECLARATION-ONLY program (no `Main`): the check must
// fire on the payload declaration regardless of whether the program has an
// entry point. This row pins that independence — s35_1_0006 ships no `Main`
// and still expects the `owned` reject.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_primitive_payload_rejected_without_entry_point_spec_28_1) {
    auto r = semaAll(
        "module Conf; "
        "choice Outcome Ready(owned code: int); end Outcome;");
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// ---- the rest of §28.1 clause 2's list, one row per named type ----------

// "`string`" — runtime-managed (§28.9), carries no obligation to transfer.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_string_payload_rejected_spec_28_1_clause_2) {
    auto r = semaAll(
        "module Conf; "
        "choice Outcome Ready(owned s: string); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// "an enum" — a value type.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_enum_payload_rejected_spec_28_1_clause_2) {
    auto r = semaAll(
        "module Conf; "
        "enum Color Red, Blue; end Color; "
        "choice Outcome Picked(owned c: Color); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// "an ordinary record" — a value type.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_record_payload_rejected_spec_28_1_clause_2) {
    auto r = semaAll(
        "module Conf; "
        "record Pair(A: int, B: int); "
        "choice Outcome Made(owned p: Pair); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// "a non-owning inline array" — slots hold borrows, so there is no per-slot
// obligation for a payload marker to convey. The owning `[N owned]T` twin is
// ADMITTED (row below), which is what makes this row a real boundary test.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_nonowning_inline_array_payload_rejected_spec_28_1_clause_2) {
    auto r = semaAll(
        "module Conf; "
        "class Res public constructor() end constructor; end Res; "
        "choice Outcome Many(owned v: [2]Res); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// "the unit type".
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_unit_payload_rejected_spec_28_1_clause_2) {
    auto r = semaAll(
        "module Conf; "
        "choice Outcome Ready(owned u: ()); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedNotAdmissible(r));
}

// §28.1 clause 5 (v0.196): a declaration whose constraints exclude class and
// interface substitutions outright (`where T: value`) rejects the marker AT
// THE DECLARATION — "it could never be more than redundant". This reaches the
// payload position through `checkOwnedDeclType`'s `hasValueConstraint` lambda,
// which reads the CHOICE's own clauses via `currentOuterType_`; the choice
// payload loop runs inside resolveTypeDecl's save/restore window, so the
// choice's `where` clause is visible at the payload check.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_value_bound_type_parameter_payload_rejected_spec_28_1_clause_5) {
    auto r = semaAll(
        "module Conf; "
        "choice Box<T> where T: value Full(owned v: T); end Box; " +
        std::string(kMain));
    BEZEL_EXPECT_TRUE(hasOwnedValueBoundReject(r));
}

// ===========================================================================
// ADMIT CONTROLS — the nearest neighbours §28.1 admits. A check that rejects
// too much is the same defect class pointed the other way (a silently denied
// valid program), so each control states what would fail if the fix were
// deleted or over-broad.
// ===========================================================================

// ADMIT CONTROL 1 — a CLASS payload stays accepted (§28.1's first admissible
// kind). WHAT WOULD FAIL IF THE CHECK WERE DELETED: nothing in this row — it is
// an admit row, and its purpose is the opposite: if the check were OVER-BROAD
// (keyed on the `owned` token alone rather than on the resolved payload type),
// THIS row turns red. The paired reject rows (primitive / string / enum /
// record / non-owning array / unit) are what fail if the check is deleted —
// they are the rows that carry the defect.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_class_payload_still_admitted_spec_28_1) {
    auto r = semaAll(
        "module Conf; "
        "class Res public constructor() end constructor; end Res; "
        "choice Outcome Ready(owned r: Res); end Outcome; "
        "class Program "
        "  public static method Main() var o := Outcome.Ready(new Res()); _ := o; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasOwnedNotAdmissible(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 2 — an INTERFACE payload stays accepted (§28.1's second
// admissible kind). Fails if the check were over-broad to reject any named
// type, or if the `Type`-specific arm leaked onto interfaces.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_interface_payload_still_admitted_spec_28_1) {
    auto r = semaAll(
        "module Conf; "
        "interface ILog method Note(); end ILog; "
        "choice Outcome Logged(owned l: ILog); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_FALSE(hasOwnedNotAdmissible(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 3 — a BARE TYPE PARAMETER payload stays accepted (§28.1's
// third admissible kind, and the per-instantiation-erasure rule of clause 3:
// "the payload slot is owned at class and interface substitutions and the
// marker is erased at value and runtime-managed substitutions"). Fails if the
// check rejected bare parameters wholesale — the same way the clause-5 row
// above pins that it must NOT reject them when bound `where T: value`, and
// MUST when they can only ever be a value. That pair is what distinguishes
// "bare parameter" from "value-constrained bare parameter"; deleting the
// `hasValueConstraint` arm makes THIS row red (an unconstrained T would be
// rejected).
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_bare_type_parameter_payload_still_admitted_spec_28_1_clause_3) {
    auto r = semaAll(
        "module Conf; "
        "class Res public constructor() end constructor; end Res; "
        "choice Box<T> Full(owned v: T); end Box; "
        "class Program "
        "  public static method Main() var b := Box<Res>.Full(new Res()); _ := b; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasOwnedNotAdmissible(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 4 — an OWNING inline-array payload stays accepted, per §28.1
// clause 1 ("the modifier is admitted-but-redundant — the type's own move
// semantics already transfer the obligation"). This is the tightest boundary
// in the fix: it differs from the non-owning reject row above by exactly the
// `[N owned]` marker, so it fails if the check ignored `arrayOwned` and
// rejected both inline-array shapes alike.
BEZEL_TEST(choice_payload_owned_type_requirement,
           owned_on_owning_inline_array_payload_admitted_redundant_spec_28_1_clause_1) {
    auto r = semaAll(
        "module Conf; "
        "class Res public constructor() end constructor; end Res; "
        "choice Outcome Many(owned v: [2 owned]Res); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_FALSE(hasOwnedNotAdmissible(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT CONTROL 5 — an UNMARKED payload of an inadmissible type is still fine.
// `owned` is what §28.1 constrains; a plain `int` payload is §35.1's ordinary
// case. Fails if the fix keyed on the payload's TYPE rather than on the
// payload's `owned` marker, which would have turned every value-typed payload
// in every choice in the corpus into an error.
BEZEL_TEST(choice_payload_owned_type_requirement,
           unmarked_primitive_payload_still_admitted_spec_35_1) {
    auto r = semaAll(
        "module Conf; "
        "choice Outcome Ready(code: int); end Outcome; " +
        std::string(kMain));
    BEZEL_EXPECT_FALSE(hasOwnedNotAdmissible(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}