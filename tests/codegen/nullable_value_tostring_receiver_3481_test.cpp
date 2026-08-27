// Regression tests for #3481 — `.ToString()` on a §7.4/§12.7 FLOW-NARROWED
// NULLABLE VALUE receiver (language spec §8.5 / §7.4 / §12.7 / §18.5, v0.283).
//
// WHAT WAS WRONG. A nullable value type lowers to the tagged
// `{i8 hasValue; T value}` struct (`isNullableValueTy`, the one predicate that
// names that representation). Two receiver-classification sites asked what KIND
// the receiver was and never asked whether it was `?`:
//
//   * `LlvmBackend_Exprs.cpp`'s CG27c arm — gated on
//     `tt->kind == Kind::Primitive`, true of `int?` exactly as of `int` — handed
//     the whole struct to `emitPrimitiveToString`;
//   * `LlvmBackend_Calls2.cpp`'s `tryLowerValueSemanticsCall` — `primValueSem` /
//     `namedValueSem` / `choiceValueSem`, gated on `kind`, `primitive` and
//     `declaration->kind`, none of which `?` changes — handed the whole struct
//     to `lowerValueSemMethodOn`.
//
// §7.4 makes every one of these calls LEGAL ("Member access, method calls, and
// indexing on a nullable value are compile errors unless the compiler has
// narrowed it to non-null"), and §7.4's own list of how narrowing is
// established includes the initializer form these programs use ("assignment:
// after `x := expr;` where `expr`'s static type is non-nullable … `x` is
// narrowed until invalidated"). The narrowing is applied at sema and was not
// applied at codegen, so these are not rejected programs — they MISCOMPILED.
//
// MEASURED ON 0c5ecd1, one statement per compile. Three distinct symptoms:
//
//   | receiver                | observed                     | §8.5 answer    |
//   |-------------------------|------------------------------|----------------|
//   | `int?    := 5`          | `1`                          | `5`            |
//   | `int?    := 12345`      | `1`                          | `12345`        |
//   | `long?   := 5`          | `1`                          | `5`            |
//   | `decimal?:= 2.5m`       | `109291918593`               | `2.5`          |
//   | `Pt?     := Pt(7, 8)`   | `Pt(X: 1, Y: 7)`             | `Pt(X: 7, Y: 8)`|
//   | `bool?   := true`       | IR-verify abort, exit 1      | `True`         |
//   | `double? := 1.5`        | IR-verify abort, exit 1      | `1.5`          |
//   | `float?  := 1.25f`      | IR-verify abort, exit 1      | `1.25`         |
//   | `char?   := 'Z'`        | IR-verify abort, exit 1      | `Z`            |
//   | `byte?   := byte(200)`  | IR-verify abort, exit 1      | `200`          |
//   | `Sh?     := Sh.Circle(3)`| compiler SIGSEGV rc=139     | `Sh.Circle(r: 3)`|
//
// ⚠ THE FIRST FIVE ROWS ARE THE DANGEROUS ONES: exit 0, and a plausible value.
// For `int?`/`long?` the rendered number is the `{i8 hasValue, T}` struct's
// hasValue TAG — `5` and `12345` both printed `1`, so the payload was
// unobservable. For the RECORD the tag is even more legible: the fields are read
// one slot late, so the rendered `X` IS the tag (`1`) and the rendered `Y` is
// the real `X`. AN EXIT-CODE ORACLE IS VACUOUS FOR THESE ROWS — every test below
// asserts exact stdout, and every payload is chosen so that no digit of it could
// be confused with the tag.
//
// The IR-verifier aborts are the same defect, not a second one: they are the
// receivers whose `emitPrimitiveToString` arm truncs/zexts/passes the value at
// its bare LLVM type, e.g.
// `Trunc only operates on integer:  %2 = trunc { i8, i1 } %1 to i1`.
//
// THE CONTRAST THAT LOCALIZED IT. Four other spellings of the SAME value were
// already correct on the pre-fix compiler and are pinned below as controls:
// `Console.WriteLine(v)`, the §18.5 hole `$"{v}"`, `v?.ToString()`, and
// `v!.ToString()`. All four already routed through `emitValueToString` — #2913's
// shared §8.5 authority, whose FIRST arm is the nullable-value tag branch. The
// fix feeds those two classification sites into it rather than adding a fourth
// spelling of the rule: site 1 delegates to `emitValueToString` with the
// NULLABLE TypeRef (so the tag branch, and §18.5's `null` text, come from the
// one place that defines them), site 2 reads the payload out and classifies on
// the non-nullable form, which is `lowerNullCondValueSemCall`'s unwrap verbatim.
//
// §8.5 IS QUOTED, not paraphrased, at each row that turns on it.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C … Main` wrapper.
std::string plain(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

// Same, with a leading type-declaration preamble.
std::string withDecls(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " class C public static method Main(): int " + body +
           "end Main; end C;";
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kPt = "record Pt(X: int, Y: int);";
const char* kColor = "enum Color Red, Green, Blue; end Color;";
const char* kColorOverride =
    "enum Color Red, Green, Blue; "
    "  public method ToString(): string => "
    "    (case this of Red => \"RED\", Green => \"GREEN\", Blue => \"BLUE\"); "
    "end Color;";
const char* kSh = "choice Sh Circle(r: int), Sq(s: int); end Sh;";
// A choice that is NOT auto-hashable (an owning payload, §35.7): its
// `.ToString()` has no compiler-defined default and must stay a DIAGNOSTIC on
// both the nullable and the non-nullable spelling.
const char* kOwningChoice =
    "class Res public property N: int; "
    "  public constructor(n: int) N := n; end constructor; end Res; "
    "choice Own Held(owned r: Res), None; end Own;";
const char* kNonAeqDiag =
    "member `ToString` on a choice that is not auto-hashable";

}  // namespace

// ── T1. The two rows that exit 0 and print the TAG ──────────────────────────
//
// §8.5's first sentence puts the rendering of a PRESENT nullable in scope:
// "`ToString` is invoked implicitly inside string interpolation ([§18.5]) via
// virtual dispatch — for present values; a `null` value of a nullable-typed hole
// renders as the text `null` with no dispatch ([§18.5])."
//
// 12345 is chosen deliberately: no digit of it is `1` alone, and the pre-fix
// output was the bare tag `1` for EVERY payload — `5` and `12345` alike — so a
// row asserting `5` could be passed by a compiler that only ever printed the
// first digit. Both payloads are pinned.

BEZEL_TEST(nullable_value_tostring_receiver_3481, int_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var v: int? := 12345; "
        "  Console.WriteLine(v.ToString()); "
        "  return 0; ")),
        "12345");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, int_nullable_small_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var v: int? := 5; "
        "  Console.WriteLine(v.ToString()); "
        "  return 0; ")),
        "5");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, long_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var w: long? := 9000000000; "
        "  Console.WriteLine(w.ToString()); "
        "  return 0; ")),
        "9000000000");
}

// ── T2. The CONCATENATED spelling ───────────────────────────────────────────
//
// A second lowering site reaches the same arm through the `+` operand chain, and
// two independent observers of this defect measured DIFFERENT garbage in the
// exact and the concatenated forms (the struct is passed by value, so what
// `snprintf`'s `%d` reads is unspecified). Both spellings are pinned, and the
// third arm — the same call inside a §18.5 hole — is pinned in T7.

BEZEL_TEST(nullable_value_tostring_receiver_3481, int_nullable_concatenated) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var v: int? := 12345; "
        "  Console.WriteLine(\"x=\" + v.ToString()); "
        "  return 0; ")),
        "x=12345");
}

// ── T3. The receivers that did not COMPILE at all ───────────────────────────
//
// `r.ran` is false for every one of these on the pre-fix compiler (the IR
// verifier aborts the build), so these rows cannot pass vacuously — runAotOut
// returns its "<did-not-run>" sentinel, which is not the expected text.

BEZEL_TEST(nullable_value_tostring_receiver_3481, bool_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var x: bool? := true; "
        "  Console.WriteLine(x.ToString()); "
        "  return 0; ")),
        "True");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, double_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var y: double? := 1.5; "
        "  Console.WriteLine(y.ToString()); "
        "  return 0; ")),
        "1.5");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, float_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var f: float? := 1.25f; "
        "  Console.WriteLine(f.ToString()); "
        "  return 0; ")),
        "1.25");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, char_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var c: char? := 'Z'; "
        "  Console.WriteLine(c.ToString()); "
        "  return 0; ")),
        "Z");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, byte_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var b: byte? := byte(200); "
        "  Console.WriteLine(b.ToString()); "
        "  return 0; ")),
        "200");
}

// `decimal?` is the third symptom: it compiled, exited 0, and printed
// `109291918593` — a number with no relation to `2.5`.
BEZEL_TEST(nullable_value_tostring_receiver_3481, decimal_nullable_renders_payload) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var d: decimal? := 2.5m; "
        "  Console.WriteLine(d.ToString()); "
        "  return 0; ")),
        "2.5");
}

// ── T4. The RECORD row — where the tag is legible in the output ─────────────
//
// §8.5, for positional records: "For positional records: a representation of
// fields, e.g. `\"Point(X: 10, Y: 20)\"`." Pre-fix this printed
// `Pt(X: 1, Y: 7)`: the fields were read one slot late, so the rendered `X` is
// the hasValue TAG and the rendered `Y` is the real `X`. 7 and 8 are chosen so
// that the shifted reading and the correct reading share no component.

BEZEL_TEST(nullable_value_tostring_receiver_3481, record_nullable_renders_fields) {
    BEZEL_EXPECT_EQ(runAotOut(withDecls(kPt,
        "  var p: Pt? := new Pt(7, 8); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; ")),
        "Pt(X: 7, Y: 8)");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, record_nullable_concatenated) {
    BEZEL_EXPECT_EQ(runAotOut(withDecls(kPt,
        "  var p: Pt? := new Pt(7, 8); "
        "  Console.WriteLine(\"v=\" + p.ToString()); "
        "  return 0; ")),
        "v=Pt(X: 7, Y: 8)");
}

// ── T5. The CHOICE row — a compiler SIGSEGV pre-fix ─────────────────────────
//
// `s.ToString()` on `Sh? := Sh.Circle(3)` made bezelc exit 139 with NO
// diagnostic on 0c5ecd1, while `$"{s}"` on the same value and `.ToString()` on
// the non-nullable spelling both rendered correctly. Both are asserted here, and
// the row asserts they AGREE — §8.5 has one answer per value, so a fix that
// changed the nullable answer to anything but the non-nullable one fails this
// row even though one rule generates both lines.

BEZEL_TEST(nullable_value_tostring_receiver_3481, choice_nullable_renders_case) {
    BEZEL_EXPECT_EQ(runAotOut(withDecls(kSh,
        "  var s: Sh? := Sh.Circle(3); "
        "  var t: Sh := Sh.Circle(3); "
        "  var a := s.ToString(); "
        "  var b := t.ToString(); "
        "  Console.WriteLine((if a = b then a else \"DISAGREE\")); "
        "  return 0; ")),
        "Sh.Circle(r: 3)");
}

// ⚠ THE DIAGNOSTIC MUST SURVIVE. A choice that is not auto-hashable has no
// compiler-defined `ToString` default (§35.7 / #2479), and the pre-fix compiler
// reported that for BOTH the nullable and the non-nullable spelling. Routing the
// nullable receiver through the §8.5 renderer instead of the value-semantics
// dispatcher would have silently answered the empty string here. Classifying on
// the UNWRAPPED type — rather than adding a nullable arm to each predicate — is
// what keeps this row a rejection.
BEZEL_TEST(nullable_value_tostring_receiver_3481,
           non_auto_hashable_choice_still_diagnosed_nullable) {
    BEZEL_EXPECT_TRUE(hasError(withDecls(kOwningChoice,
        "  var o: Own? := Own.None; "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; "), kNonAeqDiag));
}

BEZEL_TEST(nullable_value_tostring_receiver_3481,
           non_auto_hashable_choice_still_diagnosed_non_nullable) {
    BEZEL_EXPECT_TRUE(hasError(withDecls(kOwningChoice,
        "  var o: Own := Own.None; "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; "), kNonAeqDiag));
}

// ── T6. THE NULL CONTROL — must pass on BOTH compilers ──────────────────────
//
// §7.4 rejects `.ToString()` on a nullable value that has NOT been narrowed, so
// the only way a null value reaches this rendering is a guarded branch that does
// not take it. This row proves the fix did not turn "render the payload" into an
// unconditional payload read: the null local takes the `else` arm, the present
// one takes the `then` arm and renders, and both are pinned in one program with
// the values ordered so a swapped branch is visible.
//
// `C.Get()` returns `int?` from a method, so the null is not a literal the
// front-end could fold — the tag is a genuine runtime value here.

BEZEL_TEST(nullable_value_tostring_receiver_3481, runtime_null_takes_the_other_arm) {
    auto r = runAot(
        "module M; class C "
        "  public static method Get(): int? => null; "
        "  public static method Main(): int "
        "    var v: int? := C.Get(); "
        "    if v <> null then Console.WriteLine(\"present:\" + v.ToString()); "
        "    else Console.WriteLine(\"absent\"); end; "
        "    var w: int? := 31337; "
        "    if w <> null then Console.WriteLine(\"present:\" + w.ToString()); "
        "    else Console.WriteLine(\"absent\"); end; "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "absent\npresent:31337\n");
}

// Narrowing established by an explicit `<> null` TEST (§7.4's null-test form),
// not by the initializer — a second §7.4 narrowing form reaching the same site.
BEZEL_TEST(nullable_value_tostring_receiver_3481, narrowed_by_null_test_renders) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var w: int? := 4242; "
        "  if w <> null then Console.WriteLine(w.ToString()); end; "
        "  return 0; ")),
        "4242");
}

// ── T7. THE FOUR SPELLINGS THAT ALREADY WORKED — the localizing contrast ────
//
// §8.5 makes interpolation and the explicit call ONE rule ("`ToString` is
// invoked implicitly inside string interpolation"), so these must agree. Three
// of the five lines below rendered correctly on the pre-fix compiler and are
// controls; the `$"{v.ToString()}"` line is the same defect reached through a
// §18.5 hole and printed `2013742593` there — different garbage again, which is
// why the hole spelling gets its own line rather than being assumed.

BEZEL_TEST(nullable_value_tostring_receiver_3481, all_five_spellings_agree) {
    auto r = runAot(plain(
        "  var v: int? := 12345; "
        "  Console.WriteLine(v.ToString()); "
        "  Console.WriteLine($\"{v}\"); "
        "  Console.WriteLine($\"{v.ToString()}\"); "
        "  Console.WriteLine(v); "
        "  Console.WriteLine(v!.ToString()); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "12345\n12345\n12345\n12345\n12345\n");
}

// `v?.ToString()` yields `string?`, so it is a separate line rather than a sixth
// spelling above. It routes through `lowerNullCondValueSemCall`, whose unwrap
// the site-2 fix copies — it was correct pre-fix and must stay correct.
BEZEL_TEST(nullable_value_tostring_receiver_3481, null_conditional_spelling_control) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var v: int? := 12345; "
        "  var a: string? := v?.ToString(); "
        "  Console.WriteLine(a); "
        "  return 0; ")),
        "12345");
}

// ── T8. THE ROWS THAT MUST NOT MOVE ─────────────────────────────────────────
//
// A nullable REFERENCE is a plain `ptr` with a real null, NOT the tagged struct
// (`isNullableValueTy` excludes it), so `string?` never had this defect and must
// keep its identity rendering — stdlib §9.6 `String`:
// "public pure override method ToString(): string;              // returns this"
// (⚠ SPEC §9.6 is "Records and inheritance"; this rule is the STDLIB's §9.6.)
BEZEL_TEST(nullable_value_tostring_receiver_3481, nullable_string_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var z: string? := \"s\"; "
        "  Console.WriteLine(z.ToString()); "
        "  return 0; ")),
        "s");
}

// A nullable ENUM with no user `ToString` renders §8.5's name-only default —
// "For body records that do not satisfy auto-equality conditions, and for enums:
// the type's qualified name" — which reads NOTHING out of the receiver, so it
// was already correct pre-fix. It reaches the very predicate this fix edits
// (`namedValueSem`), so it is the row that proves the edit did not change the
// answer for a receiver kind that was already right.
BEZEL_TEST(nullable_value_tostring_receiver_3481, nullable_enum_default_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(withDecls(kColor,
        "  var e: Color? := Color.Blue; "
        "  var f: Color := Color.Blue; "
        "  var a := e.ToString(); "
        "  var b := f.ToString(); "
        "  Console.WriteLine((if a = b then a else \"DISAGREE\")); "
        "  return 0; ")),
        "M.Color");
}

// §10.5: a user `ToString(): string` on an enum replaces the §8.5 default, and
// this one READS the receiver (`case this of …`), so a broken unwrap would show.
// It resolves to a real MethodSymbol and therefore never enters
// `tryLowerValueSemanticsCall` at all — the row pins that the fix left that
// early-out alone.
BEZEL_TEST(nullable_value_tostring_receiver_3481, nullable_enum_override_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(withDecls(kColorOverride,
        "  var e: Color? := Color.Blue; "
        "  var f: Color := Color.Blue; "
        "  var a := e.ToString(); "
        "  var b := f.ToString(); "
        "  Console.WriteLine((if a = b then a else \"DISAGREE\")); "
        "  return 0; ")),
        "BLUE");
}

// Same question for a RECORD with a user `ToString` that reads its own fields.
BEZEL_TEST(nullable_value_tostring_receiver_3481, nullable_record_override_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(withDecls(
        "record Pt(X: int, Y: int) "
        "  public method ToString(): string => "
        "    \"X=\" + X.ToString() + \",Y=\" + Y.ToString(); end Pt;",
        "  var p: Pt? := new Pt(7, 8); "
        "  var q: Pt := new Pt(7, 8); "
        "  var a := p.ToString(); "
        "  var b := q.ToString(); "
        "  Console.WriteLine((if a = b then a else \"DISAGREE\")); "
        "  return 0; ")),
        "X=7,Y=8");
}

// The NON-nullable spellings of the primitive rows, in one program. None of
// them was ever broken; they reach the exact arm the site-1 fix now guards, so
// this is the row that would catch a fix that unwrapped unconditionally.
BEZEL_TEST(nullable_value_tostring_receiver_3481, non_nullable_primitives_unchanged) {
    auto r = runAot(plain(
        "  var i: int := 12345; var l: long := 9000000000; "
        "  var b: bool := true; var d: double := 1.5; "
        "  var c: char := 'Z'; var m: decimal := 2.5m; "
        "  Console.WriteLine(i.ToString()); "
        "  Console.WriteLine(l.ToString()); "
        "  Console.WriteLine(b.ToString()); "
        "  Console.WriteLine(d.ToString()); "
        "  Console.WriteLine(c.ToString()); "
        "  Console.WriteLine(m.ToString()); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "12345\n9000000000\nTrue\n1.5\nZ\n2.5\n");
}

// ── T9. The value-semantics SIBLINGS the same predicate gates ───────────────
//
// `tryLowerValueSemanticsCall` dispatches `GetHashCode` and `Equals` through the
// same three predicates, so correcting the classification fixes those receivers
// too. Pinned because they are now load-bearing on this fix: pre-fix,
// `p.GetHashCode()` / `p.Equals(…)` on a `Pt?` read the tagged struct as the
// record, and `v.GetHashCode()` on an `int?` degraded to the #2434
// zero-initialized fallback and printed `0`.
//
// §8.3's contract is what is asserted, not a particular hash value: equal values
// hash equal. The nullable and non-nullable spellings of the same value must
// agree, which is the only claim §8.3 makes and the only one a test may pin.

BEZEL_TEST(nullable_value_tostring_receiver_3481, nullable_record_value_semantics) {
    auto r = runAot(withDecls(kPt,
        "  var p: Pt? := new Pt(7, 8); "
        "  Console.WriteLine(p.Equals(new Pt(7, 8))); "
        "  Console.WriteLine(p.Equals(new Pt(8, 7))); "
        "  Console.WriteLine(p.GetHashCode() = (new Pt(7, 8)).GetHashCode()); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}

BEZEL_TEST(nullable_value_tostring_receiver_3481, nullable_int_gethashcode) {
    BEZEL_EXPECT_EQ(runAotOut(plain(
        "  var v: int? := 777; "
        "  var w: int := 777; "
        "  Console.WriteLine((if v.GetHashCode() = w.GetHashCode() "
        "                     then \"agree\" else \"DISAGREE\")); "
        "  return 0; ")),
        "agree");
}
