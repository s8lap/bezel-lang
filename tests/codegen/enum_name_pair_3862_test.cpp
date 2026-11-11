// fix #3862 — the compiler-provided per-enum static members `E.FromNameOrNull`
// and `E.NameOf` (spec §10.4 "Methods, properties, static members",
// v0.293 onwards; stdlib §9.9, v0.293 onwards).
//
// Spec §10.4 (v0.357) declares the tier verbatim:
//
//   public static method FromUnderlyingOrNull(value: U): E?;
//   public static method FromNameOrNull(name: string): E?;
//   public static method NameOf(value: E): string;
//
// and §10.4's "The name pair" paragraph states the behaviour this file pins:
//
//   "`FromNameOrNull` and `NameOf` are the two directions of one table, the
//   enum's **declared-identifier table**: each constant's own identifier —
//   the bare name as written, never a qualified `E.A` spelling — the table
//   §17.9's `nameof` reads at compile time and stdlib §2.13's
//   `Type.EnumNames` lists. `FromNameOrNull(name)` returns the declared
//   constant whose identifier equals `name`, or `null` when there is none;
//   the comparison is exact — ordinal code-point equality (stdlib §9.6),
//   case-sensitive, nothing trimmed — and a digit string names no constant,
//   since no identifier begins with a digit (§1); … It is **total** — it
//   never raises. `NameOf(value)` returns the identifier of the constant
//   `value` denotes; it is total because every value of an enum type is a
//   declared value (§5.4), and for a written constant `E.NameOf(E.A) =
//   nameof(E.A)` … Under duplicate constant values (§10.2) `NameOf` yields
//   the first-declared constant's identifier for a shared value and
//   `FromNameOrNull` accepts every declared identifier (all denote the one
//   value), so the pair round-trips **up to aliasing** … A `serialized
//   "<name>"` clause (§9.8) changes the constant's **encoded** form only:
//   … `NameOf`/`FromNameOrNull` — like `nameof` and `EnumNames` — never see
//   a rename."
//
// MEASURED on the base (before this fix): the two names fell through to the
// ordinary "no member" reject —
//   "no member `NameOf` on type `Color` (§11.3, §12.1)" /
//   "no member `FromNameOrNull` on type `Color` (§11.3, §12.1)" —
// which is the ergonomics-study B10 repro (#3862's second comment).
//
// The declaration-site half of §10.4's sentence ("no user declaration, a
// member or a declared constant, may override or overload any of them") is
// enforced separately by Resolver::checkEnumRestrictions — see
// tests/sema/enum_reserved_name_override_3862_test.cpp.
//
// ORACLE: `runAot` only (the in-process JIT harness is deprecated).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// §10.1: the default underlying values are 0, 1, 2 — assigned in declaration
// order from zero.
const char* kPriority = "module M; enum Priority Low, Medium, High; "
                        "end Priority; ";
}  // namespace

// ---------------------------------------------------------------------------
// §10.4 / stdlib §9.9 — `E.FromNameOrNull(name: string): E?`
// ---------------------------------------------------------------------------

// "returns the declared constant whose identifier equals `name`" — the hit
// case. The identifier is the constant's own bare name (§10.4), so "Medium"
// is `Priority.Medium` (value 1).
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_hit_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var p := Priority.FromNameOrNull(\"Medium\"); "
        "  return (if p = null then 0 else int(p!)); "
        "end Main; end C;"), 1);
}

// "… or `null` when there is none. It is **total** — it never raises."
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_unknown_is_null_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var p := Priority.FromNameOrNull(\"NoSuchConstant\"); "
        "  return (if p = null then 42 else 0); "
        "end Main; end C;"), 42);
}

// "the comparison is exact — ordinal code-point equality (stdlib §9.6),
// case-sensitive" — a case variant names no constant.
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_is_case_sensitive_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var a := Priority.FromNameOrNull(\"medium\"); "
        "  var b := Priority.FromNameOrNull(\"MEDIUM\"); "
        "  return (if a = null then 1 else 0) + (if b = null then 2 else 0); "
        "end Main; end C;"), 3);
}

// "nothing trimmed" — surrounding whitespace is part of the compared
// string, so neither variant names a constant.
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_trims_nothing_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var a := Priority.FromNameOrNull(\" Medium\"); "
        "  var b := Priority.FromNameOrNull(\"Medium \"); "
        "  return (if a = null then 1 else 0) + (if b = null then 2 else 0); "
        "end Main; end C;"), 3);
}

// "a digit string names no constant, since no identifier begins with a digit
// (§1)" — the corpus row `s10_4_0002`.
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_digit_string_is_null_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var a := Priority.FromNameOrNull(\"1\"); "
        "  var b := Priority.FromNameOrNull(\"0\"); "
        "  return (if a = null then 1 else 0) + (if b = null then 2 else 0); "
        "end Main; end C;"), 3);
}

// The empty string names no constant either (nothing trimmed, and no
// identifier is the empty string).
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_empty_string_is_null_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var a := Priority.FromNameOrNull(\"\"); "
        "  return (if a = null then 42 else 0); "
        "end Main; end C;"), 42);
}

// ---------------------------------------------------------------------------
// §10.4 / stdlib §9.9 — `E.NameOf(value: E): string`
// ---------------------------------------------------------------------------

// "NameOf(value) returns the identifier of the constant `value` denotes" —
// over EVERY declared member of the enum.
BEZEL_TEST(enum_name_pair_3862, name_of_every_member_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var a := Priority.NameOf(Priority.Low); "
        "  var b := Priority.NameOf(Priority.Medium); "
        "  var c := Priority.NameOf(Priority.High); "
        "  return (if a = \"Low\" then 1 else 0) "
        "       + (if b = \"Medium\" then 2 else 0) "
        "       + (if c = \"High\" then 4 else 0); "
        "end Main; end C;"), 7);
}

// "for a written constant `E.NameOf(E.A) = nameof(E.A)`" — the corpus row
// `s10_4_0003`, over every member.
BEZEL_TEST(enum_name_pair_3862, name_of_equals_nameof_spec_10_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var a := Priority.NameOf(Priority.Low) = nameof(Priority.Low); "
        "  var b := Priority.NameOf(Priority.Medium) = nameof(Priority.Medium); "
        "  var c := Priority.NameOf(Priority.High) = nameof(Priority.High); "
        "  return (if a then 1 else 0) + (if b then 2 else 0) "
        "       + (if c then 4 else 0); "
        "end Main; end C;"), 7);
}

// "Under duplicate constant values (§10.2) `NameOf` yields the first-declared
// constant's identifier for a shared value" — the corpus row `s10_4_0004`.
// `Legacy := 1, Current := 1` share the value 1, so `NameOf(Current)` is
// "Legacy".
BEZEL_TEST(enum_name_pair_3862, name_of_duplicate_value_is_first_declared_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Level Legacy := 1, Current := 1, Retired := 2; "
        "end Level; "
        "class C public static method Main(): int "
        "  var a := Level.NameOf(Level.Current); "
        "  var b := Level.NameOf(Level.Legacy); "
        "  var c := Level.NameOf(Level.Retired); "
        "  return (if a = \"Legacy\" then 1 else 0) "
        "       + (if b = \"Legacy\" then 2 else 0) "
        "       + (if c = \"Retired\" then 4 else 0); "
        "end Main; end C;"), 7);
}

// "`FromNameOrNull` accepts every declared identifier (all denote the one
// value)" — both aliases resolve, to the ONE shared value (§10.2).
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_accepts_every_alias_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Level Legacy := 1, Current := 1, Retired := 2; "
        "end Level; "
        "class C public static method Main(): int "
        "  var a := Level.FromNameOrNull(\"Legacy\"); "
        "  var b := Level.FromNameOrNull(\"Current\"); "
        "  return (if a = null then 0 else int(a!)) "
        "       + (if b = null then 0 else int(b!)); "
        "end Main; end C;"), 2);
}

// "the pair round-trips **up to aliasing**: `E.FromNameOrNull(E.NameOf(v)) =
// v` for every `v: E`, and `E.NameOf(E.FromNameOrNull(s)!) = s` for every `s`
// that is the first-declared identifier of its value — for an alias's
// identifier it returns the first-declared one."
//
// The FIRST law holds for every member (the round trip is the identity on
// the VALUE). The SECOND law holds for the first-declared identifier and
// deliberately FAILS for an alias's identifier — it returns the
// first-declared one, exactly as §10.4 states.
BEZEL_TEST(enum_name_pair_3862, round_trip_laws_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Level Legacy := 1, Current := 1, Retired := 2; "
        "end Level; "
        "class C public static method Main(): int "
        "  var v: Level := Level.Retired; "
        "  var r1 := Level.FromNameOrNull(Level.NameOf(v)); "
        "  var law1 := (if r1 = null then 0 else (if r1! = v then 1 else 0)); "
        "  var s := Level.FromNameOrNull(\"Legacy\")!; "
        "  var law2a := (if Level.NameOf(s) = \"Legacy\" then 1 else 0); "
        "  var s2 := Level.FromNameOrNull(\"Current\")!; "
        "  var law2b := (if Level.NameOf(s2) = \"Legacy\" then 2 else 0); "
        "  return law1 + law2a + law2b; "
        "end Main; end C;"), 4);
}

// ---------------------------------------------------------------------------
// §9.8 — a `serialized "<name>"` rename is the ENCODED form only
// ---------------------------------------------------------------------------

// "A `serialized \"<name>\"` clause (§9.8) changes the constant's **encoded**
// form only … `NameOf`/`FromNameOrNull` — like `nameof` and `EnumNames` —
// never see a rename." The renamed constant answers to its IDENTIFIER, and
// the wire spelling names no constant.
BEZEL_TEST(enum_name_pair_3862, serialized_rename_is_invisible_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "enum State: ISerializable, IDeserializable "
        "  On serialized \"on\", Off serialized \"off\"; "
        "end State; "
        "class C public static method Main(): int "
        "  var a := State.NameOf(State.On); "
        "  var b := State.FromNameOrNull(\"On\"); "
        "  var c := State.FromNameOrNull(\"on\"); "
        "  return (if a = \"On\" then 1 else 0) "
        "       + (if b = null then 0 else 2) "
        "       + (if c = null then 4 else 0); "
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// §5.4 — the OUT-OF-RANGE value is unreachable
// ---------------------------------------------------------------------------

// §5.4: "There is no value of an enum type that is not equal to a declared
// constant." The out-of-range case `NameOf` is asked about is therefore
// UNREACHABLE in a program the language admits: a CONSTANT undeclared value
// is a compile error at the cast site (§5.4), and a non-constant one raises
// OverflowException at run time (§5.4). This row pins that boundary — the
// fix must not widen any program's reach to an undeclared enum value.
BEZEL_TEST(enum_name_pair_3862, out_of_range_value_is_unreachable_spec_5_4) {
    auto r = runAot(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var p := Priority(9); "
        "  return int(p); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The run-time half of the same boundary: a non-constant undeclared value
// raises OverflowException — so no `E` value escapes the declared set.
BEZEL_TEST(enum_name_pair_3862, undeclared_value_raises_spec_5_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var n: int := 9; "
        "  try var p := Priority(n); return 1; "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// The enum's underlying-value direction still answers null for an
// UNDECLARED value that is inside the underlying type's range (§10.4's
// `FromUnderlyingOrNull` — "or `null` when there is none"), which is the
// reachable form of the out-of-range case.
BEZEL_TEST(enum_name_pair_3862, undeclared_underlying_value_is_null_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color: byte Red := 0, Green := 1, Blue := 2; "
        "end Color; "
        "class C public static method Main(): int "
        "  var v: byte := 200; "
        "  var p := Color.FromUnderlyingOrNull(v); "
        "  return (if p = null then 42 else 0); "
        "end Main; end C;"), 42);
}

// ---------------------------------------------------------------------------
// §10.4 / stdlib §9.9 — the declared signatures
// ---------------------------------------------------------------------------

// §10.4: `NameOf(value: E)` — the parameter is the ENUM TYPE, not its
// underlying integer and not `Object`. A value of another enum type is
// rejected.
//
// The diagnostic is PINNED: before the fix the call did not reach a
// parameter check at all — the name fell through to the ordinary "no member"
// reject ("no member `NameOf` on type `Color`"), so the message here
// distinguishes the fixed member from the absent one.
BEZEL_TEST(enum_name_pair_3862, name_of_rejects_a_value_of_another_enum_spec_10_4) {
    auto r = runAot(
        "module M; enum Color Red, Green; end Color; "
        "enum Level Legacy := 1, Current := 1; end Level; "
        "class C public static method Main(): int "
        "  var s := Color.NameOf(Level.Legacy); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "argument to `Color.NameOf`"));
}

// §10.4: `NameOf(value: E)` — an integer is not an enum value. The message
// is pinned for the same reason as the row above.
BEZEL_TEST(enum_name_pair_3862, name_of_rejects_an_integer_spec_10_4) {
    auto r = runAot(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var s := Priority.NameOf(1); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "argument to `Priority.NameOf`"));
}

// stdlib §9.9: "the parameter is non-nullable, so a `string?` in hand is
// narrowed first (`s!`, or a null test)" — §10.4's declared parameter type
// is `string`, not `string?`.
//
// The argument is an UN-narrowed `string?`: a `string?` that §7.4 has NOT
// narrowed (its initializer's static type is nullable, so the declaration
// establishes no fact — §7.4's "from a declaration whose initializer is such
// an `expr`"). A §7.4-narrowed read is of its non-nullable type and is
// admitted — see `from_name_or_null_admits_a_narrowed_nullable` below.
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_rejects_a_nullable_string_spec_9_9) {
    auto r = runAot(std::string(kPriority) +
        "class C "
        "  public static method Maybe(): string? => null; "
        "  public static method Main(): int "
        "    var s := C.Maybe(); "
        "    var p := Priority.FromNameOrNull(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "argument to `Priority.FromNameOrNull` is nullable but the "
           "target type is non-nullable"));
}

// §10.4: `NameOf(value: E)` — the parameter is the enum type itself, and
// it is NON-NULLABLE, so an un-narrowed `E?` reaches no parameter.
BEZEL_TEST(enum_name_pair_3862, name_of_rejects_a_nullable_enum_spec_10_4) {
    auto r = runAot(std::string(kPriority) +
        "class C "
        "  public static method Maybe(): Priority? => null; "
        "  public static method Main(): int "
        "    var v := C.Maybe(); "
        "    var n := Priority.NameOf(v); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "argument to `Priority.NameOf` is nullable but the target type "
           "is non-nullable"));
}

// §7.4: "a read of a binding §7.4 has narrowed non-null is of its
// non-nullable type" — so a §7.4-NARROWED nullable is admitted at both
// members' non-nullable parameter positions. stdlib §9.9 says so of
// `FromNameOrNull` verbatim ("a `string?` in hand is narrowed first (`s!`, or
// a null test)"), and §10.4's `E` parameter says the same of `NameOf`.
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_admits_a_narrowed_nullable) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C "
        "  public static method Maybe(): string? => \"Medium\"; "
        "  public static method Main(): int "
        "    var s := C.Maybe(); "
        "    var p := Priority.FromNameOrNull(s!); "
        "    return (if p = null then 0 else int(p!)); "
        "  end Main; "
        "end C;"), 1);
}

// The same admission by a NULL TEST (§7.4's first narrowing form) — and the
// `NameOf` side, whose parameter is the enum type itself.
BEZEL_TEST(enum_name_pair_3862, name_of_admits_a_narrowed_nullable) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C "
        "  public static method Maybe(): Priority? => Priority.Medium; "
        "  public static method Main(): int "
        "    var v := C.Maybe(); "
        "    if v <> null then "
        "      return (if Priority.NameOf(v) = \"Medium\" then 1 else 0); "
        "    end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// §10.4 declares the parameter NAME (`name` for `FromNameOrNull`); a
// named-argument mismatch is rejected.
//
// The message is PINNED: the spelling is `value := "Medium"` (a NAMED
// argument, §22.6's binder) — `value: "Medium"` is a PARSE error, so a
// row written that way would "pass" on a syntax diagnostic and prove
// nothing. The pin makes the row fail if it ever drifts back to that.
BEZEL_TEST(enum_name_pair_3862, from_name_or_null_rejects_a_wrong_parameter_name_spec_10_4) {
    auto r = runAot(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var p := Priority.FromNameOrNull(value := \"Medium\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has one parameter, named `name`"));
}

// §10.4 declares the members as CALLABLE statics; a BARE reference to one
// is not a member access the language admits — it falls through to the
// ordinary "no member" reject (the same posture
// `E.FromUnderlyingOrNull` has).
//
// The message is PINNED, and this row is deliberately NOT a discriminator
// between the base and the fix: the bare reference is rejected the same way
// before and after — the fix must not have turned the member into something
// a program can take a reference to.
BEZEL_TEST(enum_name_pair_3862, bare_member_reference_is_rejected_spec_10_4) {
    auto r = runAot(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var f := Priority.NameOf; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member `NameOf`"));
}

// ---------------------------------------------------------------------------
// §10.4 / §27.8 — a `type` ALIAS of the enum names the same members
// ---------------------------------------------------------------------------

// The alias spelling resolves to the same enum, so the compiler-provided
// tier answers through it exactly like the direct spelling (§27.8; the
// sibling `FromUnderlyingOrNull` spelling is pinned by
// tests/codegen/type_alias_enum_from_underlying_4072_test.cpp).
BEZEL_TEST(enum_name_pair_3862, alias_of_the_enum_answers_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasPriority3862 := Priority3862; "
        "enum Priority3862 Low, Medium, High; end Priority3862; "
        "class C public static method Main(): int "
        "  var a := AliasPriority3862.NameOf(Priority3862.Medium); "
        "  var b := AliasPriority3862.FromNameOrNull(\"High\"); "
        "  return (if a = \"Medium\" then 1 else 0) "
        "       + (if b = null then 0 else 2); "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// §10.3 — a non-`int` underlying type does not change the name table
// ---------------------------------------------------------------------------

// §10.3: the underlying type is the enum's REPRESENTATION, not its name
// table — a `byte`-backed enum names its constants the same way.
BEZEL_TEST(enum_name_pair_3862, byte_backed_enum_names_the_same_spec_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color3862: byte Red := 0, Green := 1, Blue := 2; "
        "end Color3862; "
        "class C public static method Main(): int "
        "  var a := Color3862.NameOf(Color3862.Blue); "
        "  var b := Color3862.FromNameOrNull(\"Green\"); "
        "  return (if a = \"Blue\" then 1 else 0) "
        "       + (if b = null then 0 else 2); "
        "end Main; end C;"), 3);
}

// A `long`-backed enum names its constants the same way, and its values
// above the `int` range do not disturb the table (§10.3 / #740).
BEZEL_TEST(enum_name_pair_3862, long_backed_enum_names_the_same_spec_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Big3862: long Small := 5000000000, Huge := 5000000001; "
        "end Big3862; "
        "class C public static method Main(): int "
        "  var a := Big3862.NameOf(Big3862.Huge); "
        "  var b := Big3862.FromNameOrNull(\"Small\"); "
        "  return (if a = \"Huge\" then 1 else 0) "
        "       + (if b = null then 0 else 2); "
        "end Main; end C;"), 3);
}
