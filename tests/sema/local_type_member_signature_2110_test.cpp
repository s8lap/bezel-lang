// Regression test for §21.10's member-signature rule (sema, v0.334).
//
// THE GOVERNING RULE. Spec §21.10 "Scope and name discipline" (v0.334):
// "The name may **not** appear in any **member signature of an enclosing or
// sibling declaration** — parameter, return, field/property/indexer,
// generic-constraint, or base/interface positions of the enclosing method
// itself, or of any member of any type declared outside the local type —
// which is a compile error naming that rule (*"a method-local type may not
// appear in a member signature"*); at those positions the name is not in
// scope, but the rule is stated so the diagnostic is the named one."
//
// Conformance row `s21_10_0008` (needle `member signature`, expect reject)
// pins this: its program declares `record Row` inside `Build` and names
// `Row` in the SIBLING method `Show`'s parameter list.
//
// PRE-FIX BEHAVIOUR (MEASURED, base r23-c28). Every member-signature
// position already REJECTED — nothing was silently admitted — but under
// some OTHER rule's diagnostic, because the member-signature rule was
// never checked at all:
//   * the SIBLING-member shape (`Show(row: Row)`, a field/property/base
//     declared after the method, `Array<Row>`) got the generic §21.10
//     SCOPE diagnostic, "local type `Row` is not in scope here …";
//   * the ENCLOSING-METHOD shape (`Build(): Row`, `Build(r: Row)`, a field
//     declared before the method, a `where T : Row` constraint) got
//     "unresolved type name `Row`", because a local type reaches
//     ctx_.module()->children only when its body is walked — which happens
//     AFTER its own method's signature, and after every earlier member's.
//
// The spec asks for the NAMED diagnostic at both, so both now report it.
//
// FIX. Two pieces, both in the Resolver (src/sema/):
//   1. `inMemberSignature_` — set at the entry of `resolveMember` and of
//      `resolveTypeDecl`'s header (base/interface list + generic
//      constraints), and cleared on entry to the two body walkers
//      (`resolveTypesInBody`, `resolveTypesInExpr`). Clearing centrally in
//      the two body walkers is what makes the flag exact rather than
//      approximate: every member body, contract clause, parameter default
//      and initializer reaches one of those two, so "flag set" is precisely
//      "this reference is in a member SIGNATURE".
//   2. `localTypeNamesInDecl_` — the names of local types declared in this
//      declaration's member bodies, pre-collected BEFORE any member
//      signature is walked. It is what lets the rule fire at the
//      enclosing-method positions, where no TypeSymbol exists yet. It is
//      consulted by the rule's check alone and never by ordinary name
//      resolution, so it cannot widen what any other lookup admits.
//
// CARVE-OUT, and why it is tested as hard as the rule. §21.10: "The
// prohibition does **not** reach the local type's **own** members … `public
// method Twin(): Pair` and `public method Merge(other: Pair): Pair` inside
// a local `record Pair` are therefore admitted." Part A below pins that as
// an ACCEPT: an over-broad fix that rejects it is a behaviour regression,
// and `Twin`/`Merge` are the spec's own example spellings.
//
// Part B pins the boundary the rule does NOT cross: an EARLIER local type
// named in a LATER local type's field type, which §21.10's field-type
// sentence admits by name and which lit row `local_type_decl_accept_2198`
// (#2198) already accepts. That row is why the check skips local-type
// members wholesale rather than comparing the resolved symbol — an
// over-broad check breaks a program the spec admits.
//
// ORACLE. runAot (the real AOT path). Each reject row asserts the SPECIFIC
// diagnostic — the needle this rule owns — not merely that compilation
// failed, so a future change that drops the member-signature rule back onto
// the generic scope/unresolved message fails these tests rather than
// passing vacuously.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The needle §21.10's member-signature rule owns, and the phrase the
// conformance row's `diagnostic:` header records.
constexpr const char* kMemberSignature =
    "may not appear in a member signature";

// ── Part A: the rule's own carve-out — ACCEPT ──
//
// The spec's literal example: a local `record Pair` whose OWN members name
// `Pair` in a member signature. Admitted before the fix, and must stay
// admitted after it. This row is the over-correction control: an
// implementation that flags a local type's own members breaks it.
BEZEL_TEST(local_type_member_signature_2110,
           own_members_may_name_the_local_type) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build(): int\n"
        "    record Pair(A: int, B: int)\n"
        "      public method Twin(): Pair\n"
        "        return new Pair(1, 2);\n"
        "      end Twin;\n"
        "      public method Merge(other: Pair): Pair\n"
        "        return new Pair(other.A + 1, other.B + 1);\n"
        "      end Merge;\n"
        "    end Pair;\n"
        "    var p := new Pair(1, 2);\n"
        "    return p.Merge(p.Twin()).A;\n"
        "  end Build;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

// ── Part B: an EARLIER local type in a later local type's field — ACCEPT ──
//
// This is the boundary the rule does NOT cross, and it is load-bearing
// rather than incidental: lit row `local_type_decl_accept_2198` (#2198)
// already pins it, so an over-broad member-signature check breaks that
// accepted program. §21.10's field-type sentence admits it explicitly — "a
// local type's field types may name only types already in scope by that
// rule's list — module-visible types, the enclosing class's nested types,
// and *earlier* local types" — and the prohibition's own rationale is "at
// those positions the name is not in scope", which an earlier local type
// inside a later local type's body does not satisfy.
//
// So the check skips local-type members wholesale (inLocalTypeMembers_),
// not by comparing the resolved symbol. A forward reference to a LATER
// local type from a local type's member stays rejected — but by §21.10's
// separate "no forward references" rule, with that rule's own message.
BEZEL_TEST(local_type_member_signature_2110,
           earlier_local_type_in_a_local_types_field_is_admitted) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    record Point(X: int, Y: int);\n"
        "    var p := new Point(1, 2);\n"
        "    var x := p.X + p.Y;\n"
        "    record Box\n"
        "      public Value: int;\n"
        "      public Inner: Point;\n"
        "    end Box;\n"
        "    return x;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

// ── Part C: the sibling-member shape — conformance row s21_10_0008 ──
//
// The row's exact program: `record Row` inside `Build`, named in the
// sibling method `Show`'s parameter list. Pre-fix this reported the §21.10
// SCOPE rule, not the member-signature one.
BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_sibling_method_parameter_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build(): string\n"
        "    record Row(Name: string);\n"
        "    var r := new Row(\"a\");\n"
        "    return r.Name;\n"
        "  end Build;\n"
        "\n"
        "  public static method Show(row: Row)\n"
        "  end Show;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

// ── Part D: the enclosing method's OWN signature — REJECT ──
//
// §21.10 names "the enclosing method itself" explicitly. This is the shape
// pre-fix reported as "unresolved type name `Row`", because the local type
// is not yet registered when its own method's signature is resolved. Two
// rows: the return type and a parameter type.
BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_enclosing_return_type_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build(): Row\n"
        "    record Row(Name: string);\n"
        "    return new Row(\"a\");\n"
        "  end Build;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_enclosing_parameter_type_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build(r: Row): int\n"
        "    record Row(Name: string);\n"
        "    return r.Name.Length;\n"
        "  end Build;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

// ── Part E: a member declared BEFORE the method — REJECT ──
//
// A field whose declared type names the local type. Resolution order puts
// this field ahead of `Build`'s body, so pre-fix it also read "unresolved
// type name". Exercises the pre-collected name set rather than the
// registered symbol.
BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_field_declared_before_the_method_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public _head: Row;\n"
        "  public static method Build(): int\n"
        "    record Row(Name: string);\n"
        "    return 1;\n"
        "  end Build;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

// ── Part F: the remaining named positions of §21.10's list — REJECT ──
//
// "parameter, return, field/property/indexer, generic-constraint, or
// base/interface positions". Parts C-E cover parameter, return and field;
// these three cover the property/indexer position, the generic-constraint
// position, and a base/interface position respectively.
BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_property_type_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build(): int\n"
        "    record Row(Name: string);\n"
        "    return 1;\n"
        "  end Build;\n"
        "  public property Head: Row => throw new Exception(\"x\");\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_generic_constraint_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build<T>() where T : Row\n"
        "    record Row(Name: string);\n"
        "    return 1;\n"
        "  end Build;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_base_interface_position_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "public class Program\n"
        "  public static method Build(): int\n"
        "    record Row(Name: string);\n"
        "    return 1;\n"
        "  end Build;\n"
        "end Program;\n"
        "public class Derived: Row\n"
        "end Derived;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

// ── Part G: a generic ARGUMENT in a member signature — REJECT ──
//
// `Array<Row>` in a sibling method's parameter. A generic argument is
// resolved through resolveType's own recursion, so it inherits the
// signature flag from its enclosing reference — this row is what stops a
// future refactor of that recursion from dropping the flag.
BEZEL_TEST(local_type_member_signature_2110,
           spec_21_10_generic_argument_reports_the_rule) {
    const std::string src =
        "module Conf;\n"
        "import System.Collections.Generic;\n"
        "public class Program\n"
        "  public static method Build(): int\n"
        "    record Row(Name: string);\n"
        "    return 1;\n"
        "  end Build;\n"
        "  public static method Use(items: Array<Row>): int\n"
        "    return items.Count;\n"
        "  end Use;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kMemberSignature));
}

// ── Part H: the body positions §21.10 ADMITS stay admitted — ACCEPT ──
//
// The rule is scoped to member SIGNATURES. Its own body-position list —
// local-variable ascription, `new`, a generic argument, `typeof`/`nameof`/
// `default`, a cast, a type pattern, a lambda-parameter ascription — is
// untouched by this fix, and a fix that cleared the flag at the wrong place
// (or not at all) would reject these. Same program shape as Part C, but the
// reference is in the declaring method's body rather than a signature.
BEZEL_TEST(local_type_member_signature_2110,
           body_positions_of_the_declaring_method_stay_admitted) {
    const std::string src =
        "module Conf;\n"
        "import System.Collections.Generic;\n"
        "public class Program\n"
        "  public static method Build(): int\n"
        "    record Row(Name: string);\n"
        "    var r := new Row(\"a\");\n"
        "    var rows: Array<Row> := [r];\n"
        "    var n := nameof(Row);\n"
        "    return rows.Count + n.Length + r.Name.Length;\n"
        "  end Build;\n"
        "end Program;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

}  // namespace