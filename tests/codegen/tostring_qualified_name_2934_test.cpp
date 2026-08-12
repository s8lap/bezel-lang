// Regression tests for #2934 (language spec §8.5, v0.252; stdlib §2.13
// `Type.FullName`): §8.5's three *name-only* `ToString` defaults render the
// MODULE-PATH-QUALIFIED name, and the record heads do not.
//
// THE RULING (owner, v0.241, PR #3043 — "Reading 1-lite"). §8.5's word
// "qualified" is pinned to the rendering stdlib §2.13 defines normatively for
// `Type.FullName`: "the module-path-qualified rendering stdlib §2.13 defines
// for `Type.FullName` (`"BankingApp.BankAccount"`) … The default and `FullName`
// are one rendering contract, not two." It applies to exactly three bullets:
//
//   * "For classes: the type's qualified name"
//   * "for … enums: the type's qualified name"
//   * "For body records that do not satisfy auto-equality conditions … the
//      type's qualified name"
//
// and NOT to a record head, which §8.5 records as a deliberate asymmetry:
// "positional-record and auto-equality formatting keeps the UNQUALIFIED head
// (`"Point(X: 10, Y: 20)"`, §9.1) — a record rendering already identifies
// itself by shape, while a bare class or enum default is *only* a name and
// therefore carries the module path."
//
// WHAT WAS WRONG. The compiler rendered the UNQUALIFIED identifier in all three
// name-only defaults — measured in #2934 on `c7404bf` and re-measured on
// `663a68e` for this fix: `a.ToString()` → "BankAccount" (§8.5 says
// "BankingApp.BankAccount"), `col.ToString()` → "Color", and an ineligible body
// record → "Manual". Record heads were already correct.
//
// WHY IT MATTERS BEYOND CONFORMANCE. It is the default rendering of every
// object in every diagnostic and log line a Bezel program writes, and two
// same-named types from different modules rendered identically — which is
// exactly what module qualification exists to prevent.
//
// EVERY CONSUMER MUST AGREE, so the tests below pin all of them rather than the
// explicit call alone: the direct `.ToString()` call, the §18.5 interpolation
// hole, `Console.WriteLine(obj)`, the boxed/`Object` dispatch, an interface
// receiver, `base.ToString()`, and a component rendering inside a record's own
// (unqualified-headed) output. Each also has its negative twin: a user
// `ToString` override still wins, and the record heads still do not carry the
// module path.
//
// Mechanical verification (see the commit message): reverting
// `spec85QualifiedTypeName` / `spec85NameOnlyDefaultSpelling` to `ts->name` at
// the eight emission sites leaves every "…_is_module_qualified" test below
// failing with the bare identifier, while every "…_stays_unqualified" /
// "…_unchanged" control passes either way.
//
// runAotOut moves the observable to stdout and strips the trailing line
// ending, so the expectations are the bare rendered string.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// Print one expression and return its rendered text.
std::string out(const std::string& prog) { return runAotOut(prog); }

// The #2934 report's own declarations, in its own module.
const char* kBankingApp =
    "module BankingApp; "
    "public class BankAccount public Id: int := 1; end BankAccount; "
    "public record Pt(X: int, Y: int); "
    "public enum Color Red, Green; end Color; ";

}  // namespace

// ---------------------------------------------------------------------------
// §8.5 bullet 1 — "For classes: the type's qualified name".
// ---------------------------------------------------------------------------

// The issue's own repro. Was "BankAccount".
BEZEL_TEST(tostring_qualified_2934, class_default_call_is_module_qualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  using var a := new BankAccount(); "
        "  Console.WriteLine(a.ToString()); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.BankAccount");
}

// §18.5 dispatches `ToString` implicitly, so the hole must render the same
// string the explicit call does — the property #2934 asked to be preserved.
BEZEL_TEST(tostring_qualified_2934, class_interpolation_agrees_with_call) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  using var a := new BankAccount(); "
        "  var i := $\"{a}\"; "
        "  var e := a.ToString(); "
        "  Console.WriteLine((if i = e then i else \"DISAGREE\")); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.BankAccount");
}

// stdlib §2.1 `Console.WriteLine(value)` renders through the same default.
BEZEL_TEST(tostring_qualified_2934, class_console_argument_is_module_qualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  using var a := new BankAccount(); "
        "  Console.WriteLine(a); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.BankAccount");
}

// §8.6 boxing / §8.1 `Object`: the vtable slot-3 default is the same string.
BEZEL_TEST(tostring_qualified_2934, class_object_slot_dispatch_is_qualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  using var a := new BankAccount(); "
        "  var o: Object := a; "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.BankAccount");
}

// §12 dispatch through an interface receiver reaches the same slot.
BEZEL_TEST(tostring_qualified_2934, class_interface_receiver_is_qualified) {
    BEZEL_EXPECT_EQ(out(
        "module Shapes; "
        "public interface IShape method Area(): int; end IShape; "
        "public class Square: IShape "
        "  public method Area(): int => 4; "
        "end Square; "
        "public class C public static method Main(): int "
        "  using var s := new Square(); "
        "  var i: IShape := s; "
        "  Console.WriteLine(i.ToString()); "
        "  return 0; "
        "end Main; end C;"), "Shapes.Square");
}

// §12.3 `base.ToString()` with no ancestor override is the §8.5 class default
// of the RUNTIME type — and must carry the module path like every other
// spelling of that default (#1106's path).
BEZEL_TEST(tostring_qualified_2934, base_call_default_is_module_qualified) {
    BEZEL_EXPECT_EQ(out(
        "module Deep; "
        "public class Aa public override method ToString(): string "
        "  => base.ToString(); end Aa; "
        "public class C public static method Main(): int "
        "  using var a := new Aa(); "
        "  Console.WriteLine(a.ToString()); "
        "  return 0; "
        "end Main; end C;"), "Deep.Aa");
}

// §2.1: every segment of a dotted module path is carried (stdlib §2.13's
// "qualified by its declaring module path").
BEZEL_TEST(tostring_qualified_2934, dotted_module_path_is_carried) {
    BEZEL_EXPECT_EQ(out(
        "module BankingApp.Transactions; "
        "public class Ledger public N: int := 0; end Ledger; "
        "public class C public static method Main(): int "
        "  using var l := new Ledger(); "
        "  Console.WriteLine(l.ToString()); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.Transactions.Ledger");
}

// CONTROL (§8.5 "via virtual dispatch"): a user `ToString` override still wins
// — the pin changes the DEFAULT, never a declared member.
BEZEL_TEST(tostring_qualified_2934, class_override_unchanged) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "public class K public override method ToString(): string => \"mine\"; "
        "end K; "
        "public class C public static method Main(): int "
        "  using var k := new K(); "
        "  Console.WriteLine(k.ToString()); "
        "  return 0; "
        "end Main; end C;"), "mine");
}

// ---------------------------------------------------------------------------
// §8.5 bullet 4a — "for … enums: the type's qualified name".
// ---------------------------------------------------------------------------

BEZEL_TEST(tostring_qualified_2934, enum_default_call_is_module_qualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  var col := Color.Green; "
        "  Console.WriteLine(col.ToString()); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.Color");
}

BEZEL_TEST(tostring_qualified_2934, enum_interpolation_agrees_with_call) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  var col := Color.Red; "
        "  var i := $\"{col}\"; "
        "  var e := col.ToString(); "
        "  Console.WriteLine((if i = e then i else \"DISAGREE\")); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.Color");
}

// §8.6: a boxed enum dispatches through the box thunk — same string.
BEZEL_TEST(tostring_qualified_2934, enum_boxed_dispatch_is_qualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  var col := Color.Red; "
        "  var o: Object := col; "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; "
        "end Main; end C;"), "BankingApp.Color");
}

// CONTROL (§10.5): an enum's own plain `ToString(): string` replaces the
// default for every consumer, unchanged by the pin.
BEZEL_TEST(tostring_qualified_2934, enum_user_tostring_unchanged) {
    BEZEL_EXPECT_EQ(out(
        "module M; "
        "public enum E A, B; public method ToString(): string => \"E!\"; "
        "end E; "
        "public class C public static method Main(): int "
        "  var e := E.A; "
        "  Console.WriteLine(e.ToString()); "
        "  return 0; "
        "end Main; end C;"), "E!");
}

// ---------------------------------------------------------------------------
// §8.5 bullet 4b — a body record that does NOT satisfy §9.2's auto-equality
// conditions gets the qualified name (an explicit field declaration is §9.2's
// own disqualifier).
// ---------------------------------------------------------------------------

BEZEL_TEST(tostring_qualified_2934, ineligible_body_record_is_module_qualified) {
    BEZEL_EXPECT_EQ(out(
        "module Reports; "
        "public record Manual "
        "  public property A: int; "
        "  private _cache: int := 0; "
        "  public constructor(a: int) A := a; end constructor; "
        "end Manual; "
        "public class C public static method Main(): int "
        "  var m := new Manual(3); "
        "  Console.WriteLine(m.ToString()); "
        "  return 0; "
        "end Main; end C;"), "Reports.Manual");
}

BEZEL_TEST(tostring_qualified_2934,
           ineligible_body_record_interpolation_agrees) {
    BEZEL_EXPECT_EQ(out(
        "module Reports; "
        "public record Manual "
        "  public property A: int; "
        "  private _cache: int := 0; "
        "  public constructor(a: int) A := a; end constructor; "
        "end Manual; "
        "public class C public static method Main(): int "
        "  var m := new Manual(3); "
        "  var i := $\"{m}\"; "
        "  var e := m.ToString(); "
        "  Console.WriteLine((if i = e then i else \"DISAGREE\")); "
        "  return 0; "
        "end Main; end C;"), "Reports.Manual");
}

// ---------------------------------------------------------------------------
// THE OTHER SIDE OF THE THREE-WAY SPLIT — record heads stay UNQUALIFIED.
// An over-broad flip that qualified these is the obvious failure mode of this
// fix, so both record forms are pinned explicitly.
// ---------------------------------------------------------------------------

// §8.5 / §9.1: "a representation of fields, e.g. `"Point(X: 10, Y: 20)"`" —
// the head is the bare declared identifier, NOT "BankingApp.Pt(...)".
BEZEL_TEST(tostring_qualified_2934, positional_record_head_stays_unqualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  var p := new Pt(10, 20); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; "
        "end Main; end C;"), "Pt(X: 10, Y: 20)");
}

BEZEL_TEST(tostring_qualified_2934,
           positional_record_interpolated_head_stays_unqualified) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public class C public static method Main(): int "
        "  var p := new Pt(10, 20); "
        "  Console.WriteLine($\"{p}\"); "
        "  return 0; "
        "end Main; end C;"), "Pt(X: 10, Y: 20)");
}

// §8.5 / §9.2: the auto-equality body record's property listing keeps the
// unqualified head for the same recorded reason.
BEZEL_TEST(tostring_qualified_2934,
           auto_equality_body_record_head_stays_unqualified) {
    BEZEL_EXPECT_EQ(out(
        "module People; "
        "public record Person "
        "  public property Age: int; "
        "  public constructor(a: int) Age := a; end constructor; "
        "end Person; "
        "public class C public static method Main(): int "
        "  var p := new Person(41); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; "
        "end Main; end C;"), "Person(Age: 41)");
}

// ---------------------------------------------------------------------------
// §8.5's component rule — "Each component renders as its own `ToString`
// renders it". An ENUM-typed component therefore renders the ENUM's default,
// i.e. the qualified type name, INSIDE a record rendering whose own head is
// unqualified. Both halves of the asymmetry in one string.
// ---------------------------------------------------------------------------

BEZEL_TEST(tostring_qualified_2934, enum_component_renders_qualified_in_record) {
    BEZEL_EXPECT_EQ(out(std::string(kBankingApp) +
        "public record Tagged(K: Color, N: int); "
        "public class C public static method Main(): int "
        "  var t := new Tagged(Color.Red, 5); "
        "  Console.WriteLine(t.ToString()); "
        "  return 0; "
        "end Main; end C;"), "Tagged(K: BankingApp.Color, N: 5)");
}
