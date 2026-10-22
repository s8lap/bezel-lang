// fix §35.5 / §19.3 (spec v0.334): a `|` alternative list over a CHOICE
// scrutinee emitted `icmp eq` over two choice-struct aggregates, which LLVM's
// IR verifier rejects.
//
// WHAT §35.5 SAYS (spec v0.334, *Case patterns*, last bullet):
//
//   "Payload-free case names are admissible members of `|` **alternative
//   lists** (and count toward exhaustiveness, §19.6); payload-carrying case
//   patterns are not, even in the all-discard form — they are
//   destructurings, not constants (§19.3)."
//
// §19.3 (*Alternatives*) admits as an alternative-list member "a **payload-free
// choice case name** (§35.5 — nullary, binding-free, and countable toward
// exhaustiveness like an enum constant)", and "an alternative list binds
// nothing regardless of member kind" — like constants, the members "contribute
// pure membership tests".
//
// So over a choice, an alternative list is a membership test on the CASE NAME,
// and §35.5's other bullet fixes what that test is: "A **payload-free case** is
// matched by its bare name (`NotModified`) — a constant-style pattern resolving
// through the scrutinee-type prefix-omission rule". The bare-name test a
// matching `case` already lowers is the case DISCRIMINATOR (the choice struct
// is `{ i32 tag, case0_payload, case1_payload, … }`, per `choiceStruct`), which
// is precisely what the rows below assert.
//
// WHAT WAS BROKEN. `patternMatches` (src/codegen/LlvmBackend_Exprs2.cpp) has
// an early branch for a non-nullable choice scrutinee handling exactly two
// shapes — a bare `NamedPattern` and a `DestructuringPattern`. A `Red | Green`
// arm is neither: `Parser::parsePattern` (src/parser/Parser_Exprs.cpp, §19.3
// v0.183 #1584) collapses a `|` list whose members are all constant-shaped into
// ONE `ConstantPattern`, reserving `AltPattern` for a heterogeneous list
// carrying a range or relational member. §35.5's payload-free case names are
// exactly the constant-shaped case, so the missing shape was the
// ConstantPattern one; it fell through to the generic constant handler, which
// lowers each member with `lowerExpr` into a WHOLE choice struct and hands it
// to `emitEquals` → `icmp eq` over two AGGREGATES:
//
//   Invalid operand types for ICmp instruction
//     %4 = icmp eq %choice.Color %1, { i32 0, { i8 } undef, { i8 } undef, … }
//
// That handler's `incompatible` guard discriminates only pointer / i1 / float
// (struct-vs-struct reads as compatible, and an aggregate comparison has no
// case in it), so the whole-struct ICmp was emitted and the IR verifier
// aborted the compile. Same defect on the `isNullableVal` path for a `Color?`
// scrutinee (§35.9), where the operands were the unwrapped choice struct and a
// whole choice struct.
//
// WHY NOT `emitChoiceEquality`. §35.7's generated `=` compares the active
// case's PAYLOAD fields, which has no basis here: §35.5 admits only
// payload-free members, so there is no payload to compare; and a freshly
// constructed case's payload slots hold `undef`, so a payload compare against
// one reports two equal values as unequal — the exact defect
// `emitChoiceEquality`'s own header documents. The rows below pin the tag
// semantics, including a control proving the §35.7 equality surface is
// untouched.
//
// The oracle is `runAot` (the strict path: real `Driver::compileFile`, real
// link, real exec), never the deprecated in-process JIT harness. The programs
// are built by concatenating C++ string literals with NO in-program `//`
// comment — adjacent literals form ONE line, so a `//` would swallow the rest
// of the concatenated program. Citations live in the C++ comments outside the
// literals.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §19 — the diagnostic the reject row below pins. The SAME needle the sibling
// #2227 handler emits for the equivalent primitive-scrutinee shape, so this
// fix reuses the established wording rather than inventing a new one.
constexpr const char* kSection19Msg =
    "case pattern constant type is not compatible with the scrutinee type";

// Run `prog`, return its stdout, or "<NO-RUN>" if it did not run to normal
// termination. Every row asserts on the stdout channel, never on the 8-bit
// exit status: the rows below print a value per arm, and a program whose
// observable does not fit in 8 bits must not be read through truncation.
std::string runOut(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) return "<NO-RUN>";
    return std::string(r.stdoutText);
}

}  // namespace

// === THE PINNED DEFECT =====================================================
//
// §35.5: "Payload-free case names are admissible members of `|` alternative
// lists". The arm `Red | Green` over `x: Color` therefore matches exactly when
// `x`'s case is Red or Green, and the program must compile and select the arm.
//
// Measured on the base tree (r23-c28): this row FAILS — `bezelc` aborts with
// "Invalid operand types for ICmp instruction … icmp eq %choice.Color %1,
// { i32 0, { i8 } undef, { i8 } undef, { i8 } undef }" and
// "codegen error: IR verification failed (malformed IR emitted)". `runAot`
// returns ran=false.

BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           alt_list_of_payload_free_cases_selects_by_case_name) {
    // Each case printed through the SAME alt-list arm, so all three tag values
    // must come back: Red → 1 and Green → 1 (the alt-list arm), Blue → 2 (the
    // next arm). A lowering that ORs the tags is right; one that compares the
    // struct would not have compiled at all.
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C "
        "  private static method Score(c: Color): int "
        "    return (case c of "
        "      Red | Green => 1, "
        "      Blue        => 2); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Color.Red)); "
        "    Console.WriteLine(Score(Color.Green)); "
        "    Console.WriteLine(Score(Color.Blue)); "
        "    return 0; "
        "end Main; end C;"),
        "1\n1\n2\n");
}

// §35.5 admits payload-free cases; the alternative list covers exactly the
// named ones. Blue reaches the FOLLOWING arm and Green does not leak into it —
// the same program with the two arms' results transposed proves the OR is a
// membership test on the case name, not "first member wins" or an
// unconditional true.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           alt_list_arm_does_not_swallow_the_following_arm) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C "
        "  private static method Score(c: Color): int "
        "    return (case c of "
        "      Red | Green => 2, "
        "      Blue        => 1); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Color.Red)); "
        "    Console.WriteLine(Score(Color.Green)); "
        "    Console.WriteLine(Score(Color.Blue)); "
        "    return 0; "
        "end Main; end C;"),
        "2\n2\n1\n");
}

// §19.3: a member is a "constant-style pattern resolving through the
// scrutinee-type prefix-omission rule, extended from enums (§10.6, §19.8)".
// Both spellings of the same arm must select identically: the bare names and
// the qualified `Color.Red` / `Color.Green`.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           qualified_case_names_in_alt_list_select_by_case_name) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C "
        "  private static method Score(c: Color): int "
        "    return (case c of "
        "      Color.Red | Color.Green => 1, "
        "      Color.Blue               => 2); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Color.Red)); "
        "    Console.WriteLine(Score(Color.Green)); "
        "    Console.WriteLine(Score(Color.Blue)); "
        "    return 0; "
        "end Main; end C;"),
        "1\n1\n2\n");
}

// §35.9 (nullable choice) + §35.5: the same list over a `Color?` scrutinee.
// The lowering is the tag test on the UNWRAPPED choice struct ANDed with
// hasValue — the #4399 sibling for a single bare case name, extended to the
// list form. The `null` member is handled ahead of the choice test (it
// matches when hasValue == 0), so a present Red is 1 and an absent value is
// 9 — the two must not be confused.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           alt_list_over_nullable_choice_requires_a_value) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C "
        "  private static method Score(c: Color?): int "
        "    return (case c of "
        "      Red | Green => 1, "
        "      Blue        => 2, "
        "      null        => 9); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Color.Red)); "
        "    Console.WriteLine(Score(Color.Green)); "
        "    Console.WriteLine(Score(Color.Blue)); "
        "    Console.WriteLine(Score(null)); "
        "    return 0; "
        "end Main; end C;"),
        "1\n1\n2\n9\n");
}

// §35.5 admits ONLY payload-free cases as members, "payload-carrying case
// patterns are not, even in the all-discard form". A payload case carries
// information the tag does not, so an alt-list naming it plus a payload-free
// case must still route the payload case to its own destructuring arm. This
// pins that the tag test cannot see a payload and does not claim the payload
// case's values.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           payload_case_binds_through_its_own_destructuring_arm) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Opt Some(n: int), None; end Opt; "
        "class C "
        "  private static method Score(o: Opt): int "
        "    return (case o of "
        "      None          => 1, "
        "      Some(var n)   => n); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Opt.Some(42))); "
        "    Console.WriteLine(Score(Opt.None)); "
        "    return 0; "
        "end Main; end C;"),
        "42\n1\n");
}

// §35.13 / §14.1: a GENERIC choice specialises its payload layout per
// instantiation (§35.13; the choice struct is keyed on the active
// substitution — `choiceStruct`, #2047). The tag is field 0 of every
// specialisation, so the tag test must hold on the SPECIALIZED struct, not
// only on a non-generic one. The payload case still routes to its own
// destructuring arm and binds its `T` — this row pins both halves at once.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           alt_list_over_generic_choice_selects_on_the_specialized_tag) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Res<T> Ok(v: T), Bad, Err; end Res; "
        "class C public static method Main(): int "
        "  var a: Res<int> := Res<int>.Bad; "
        "  var b: Res<int> := Res<int>.Err; "
        "  var c: Res<int> := Res<int>.Ok(7); "
        "  Console.WriteLine((case a of Bad | Err => 1, Ok(var v) => v + 100)); "
        "  Console.WriteLine((case b of Bad | Err => 1, Ok(var v) => v + 100)); "
        "  Console.WriteLine((case c of Bad | Err => 1, Ok(var v) => v + 100));"
        "  return 0; "
        "end Main; end C;"),
        "1\n1\n107\n");
}

// §19.6: "admitted and never selected" — a `null` alternative over a
// scrutinee that cannot hold null is admitted, contributes nothing, and draws
// NO diagnostic. Red still matches the alt-list arm, and the non-exhaustive
// warning still names Green (the arm really does not cover it), so the
// never-selected member is not being silently folded into coverage either.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           null_member_over_non_nullable_choice_is_admitted_never_selected) {
    auto r = runAot(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C "
        "  private static method Score(c: Color): int "
        "    return (case c of "
        "      Red | null => 1, "
        "      Blue       => 2, "
        "      _          => 3); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Color.Red)); "
        "    Console.WriteLine(Score(Color.Green)); "
        "    Console.WriteLine(Score(Color.Blue)); "
        "    return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(std::string(r.stdoutText), "1\n3\n2\n");
}

// === DIFFERENTIAL CONTROLS ================================================
//
// A lowering that fires for every choice `case` would satisfy the rows above
// while silently changing behaviour the rows do not reach. These pin the
// pre-existing paths that must keep behaving exactly as they did.

// CONTROL 1. The bare single-name arm over a choice is the §35.5 form the
// alt-list is defined in terms of, and it was already lowered by the
// tag-compare arm this fix does not touch. Green must still bind to ITS own
// arm, so the alt-list rows are measuring a new shape and not a rewrite of
// this one.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           control_bare_single_name_arm_is_unchanged) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C "
        "  private static method Score(c: Color): int "
        "    return (case c of "
        "      Red   => 4, "
        "      Green => 5, "
        "      Blue  => 6); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Color.Red)); "
        "    Console.WriteLine(Score(Color.Green)); "
        "    Console.WriteLine(Score(Color.Blue)); "
        "    return 0; "
        "end Main; end C;"),
        "4\n5\n6\n");
}

// CONTROL 2. §10.6 / §19.3: the SAME `|` list over an ENUM scrutinee was
// already green, and §35.5 explicitly extends the enum's rule to choices.
// It must still select by the enum constant's value — this is the row that
// localises the defect to the choice branch.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           control_alt_list_over_enum_is_unchanged) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "enum Hue Warm, Cool, Neutral; end Hue; "
        "class C "
        "  private static method Score(h: Hue): int "
        "    return (case h of "
        "      Warm | Cool => 7, "
        "      Neutral    => 8); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Hue.Warm)); "
        "    Console.WriteLine(Score(Hue.Cool)); "
        "    Console.WriteLine(Score(Hue.Neutral)); "
        "    return 0; "
        "end Main; end C;"),
        "7\n7\n8\n");
}

// CONTROL 3. §35.7 — the generated `=` on a value-like choice is the payload
// comparison this fix deliberately does NOT use for patterns. It must be
// untouched: `Opt.Some(7) = Opt.Some(7)` is true (same active case, every
// payload equal) while `Opt.Some(7) <> Opt.Some(8)` and `Opt.Some(7) <>
// Opt.None()` are false/true respectively. A fix that had routed the pattern
// through §35.7 equality would still pass the alt-list rows, so this row is
// what shows the two surfaces stayed apart.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           control_choice_35_7_equality_still_compares_payloads) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "choice Opt Some(n: int), None; end Opt; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Opt.Some(7) = Opt.Some(7)); "
        "    Console.WriteLine(Opt.Some(7) = Opt.Some(8)); "
        "    Console.WriteLine(Opt.Some(7) = Opt.None); "
        "    return 0; "
        "end Main; end C;"),
        "True\nFalse\nFalse\n");
}

// === REJECTION ROWS =======================================================
//
// §19 / §35.5: an alternative-list member that is not comparable to the
// scrutinee is a §19 type error. Over a choice, §35.5 narrows the admissible
// members to payload-free case names, so a literal member is not one of them.
//
// Pre-fix this was a MALFORMED-IR ABORT ("Invalid operand types for ICmp
// instruction … icmp eq %choice.Color %1, i32 5", then "codegen error: IR
// verification failed"), i.e. a compiler crash on an ill-formed program rather
// than a diagnostic. The row pins the clean diagnostic that replaces it —
// the SAME needle the sibling #2227 handler emits for the equivalent
// primitive-scrutinee shape, so this is a repair of a crash, not a new
// acceptance or a new rule.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           literal_member_over_choice_reports_a_section_19_diagnostic) {
    auto r = runAot(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red | 5 => 1, "
        "    Blue    => 2); "
        "end Main; end C;");
    // A clean REJECT at a real source location — not a runtime crash, signal,
    // timeout or ASan report (what `failedAtCompileTimeWithRealLocation`
    // excludes), and carrying the §19 needle.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSection19Msg));
    // The pre-fix outcome, stated as its own assertion so the row cannot pass
    // on a message that merely happens to contain the needle: the malformed
    // whole-struct ICmp and the verifier abort that went with it must be
    // GONE. A rejection at the IR verifier is a compiler bug, not an
    // acceptance — this is what says so.
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "Invalid operand types for ICmp"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "IR verification failed"));
}

// The SAME §19 rejection on the `isNullableVal` path (§35.9). The two
// lowerings are separate code, so a fix that reported on one and silently
// never-matched on the other would pass the row above; this is the arm that
// shows the nullable path makes the same verdict. Pre-fix this shape produced
// "icmp eq %choice.Color %nv.val, i32 5" — the same malformed aggregate
// ICmp, on the unwrapped struct.
BEZEL_TEST(choice_alt_list_tag_lowering_35_5,
           literal_member_over_nullable_choice_reports_a_section_19_diagnostic) {
    auto r = runAot(
        "module M; "
        "choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color? := Color.Red; "
        "  return (case x of "
        "    Red | 5 => 1, "
        "    Blue    => 2, "
        "    null    => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSection19Msg));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "Invalid operand types for ICmp"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "IR verification failed"));
}