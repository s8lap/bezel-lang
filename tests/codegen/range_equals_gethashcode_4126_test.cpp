// Regression tests for issue #4126, the `Range<T>` rows: `r.Equals(other)`
// and `r.GetHashCode()` on a range VALUE.
//
// stdlib §9.8 (v0.338): "`Range<T>` is a compiler-intrinsic record" and "All
// six declared properties are the type's auto-property surface for the
// language's generated members (spec §8.5/§9.2)". Language spec §8.3 (v0.338),
// body-only record row: "property-by-property value equality", with
// "`GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() =
// b.GetHashCode()`" and an order-sensitive combiner. §8.4: "`a = b` invokes
// `a.Equals(b)`". §9.2 (*Dispatch modifiers on value kinds*): the own-typed
// `Equals` fills the kind's `Object` slot, and through the slot "the member
// runs only on a `T` payload and any other argument compares unequal without
// the body running".
//
// Measured before the fix (ea54f59e): both members degraded to a
// zero-initialized value behind an `internal:` warning — `r1.Equals(r2)` for
// two `1..5` answered False (while `r1 = r2` answered True), `GetHashCode()`
// answered 0, and a `Console.WriteLine` of either printed nothing at all.
// `SemaContext::builtinClassSym` stamps `Range` as a `Class`, so the record
// dispatch never saw it, and the reference dispatch needs a pointer receiver
// where a range lowers to the `%bz.range` value.
//
// The earlier `narrow #4126` (no_override_identity_builtins_4126_test.cpp)
// fixed Stopwatch / CancellationToken / Guid.GetHashCode and named these rows
// as its survivors.

#include "support/aot_support.hpp"  // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// stdout of a program that must compile, run and exit 0. A row that did not
// run reads as a marker no expectation equals.
std::string out(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

int rc(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── The issue's own repro, in the three statement spellings it measured ────

BEZEL_TEST(range_equals_gethashcode_4126,
           equal_ranges_equals_in_if_condition_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(42, rc("    var r1: Range<int> := 1..5;\n"
                           "    var r2: Range<int> := 1..5;\n"
                           "    if r1.Equals(r2) then return 42; end;\n"
                           "    return 1;\n"));
}

BEZEL_TEST(range_equals_gethashcode_4126,
           equal_ranges_equals_as_local_initializer_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(42, rc("    var r1: Range<int> := 1..5;\n"
                           "    var r2: Range<int> := 1..5;\n"
                           "    var e: bool := r1.Equals(r2);\n"
                           "    if e then return 42; end;\n"
                           "    return 1;\n"));
}

BEZEL_TEST(range_equals_gethashcode_4126,
           equals_as_writeline_argument_prints_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\n"),
                    out("    var r1: Range<int> := 1..5;\n"
                        "    var r2: Range<int> := 1..5;\n"
                        "    var r3: Range<int> := 1..6;\n"
                        "    Console.WriteLine(r1.Equals(r2));\n"
                        "    Console.WriteLine(r1.Equals(r3));\n"
                        "    return 0;\n"));
}

// ── Property-by-property: each of the four stored properties decides ───────

BEZEL_TEST(range_equals_gethashcode_4126,
           each_stored_property_distinguishes_spec_8_3_stdlib_9_8) {
    // Start, End, IsEndInclusive, Step differ in turn; the last row is equal.
    BEZEL_EXPECT_EQ(std::string("False\nFalse\nFalse\nFalse\nTrue\n"),
                    out("    var r: Range<int> := 1..5;\n"
                        "    var a: Range<int> := 2..5;\n"
                        "    var b: Range<int> := 1..6;\n"
                        "    var c: Range<int> := 1..<5;\n"
                        "    var d: Range<int> := 1..5 step 2;\n"
                        "    var e: Range<int> := 1..5 step 1;\n"
                        "    Console.WriteLine(r.Equals(a));\n"
                        "    Console.WriteLine(r.Equals(b));\n"
                        "    Console.WriteLine(r.Equals(c));\n"
                        "    Console.WriteLine(r.Equals(d));\n"
                        "    Console.WriteLine(r.Equals(e));\n"
                        "    return 0;\n"));
}

// §16.3: "a `null` endpoint indicates the range is open-ended on that side".
// `..5` (Start null) and `0..5` (Start 0) differ in `Start`, so are unequal.
BEZEL_TEST(range_equals_gethashcode_4126,
           open_ended_null_endpoint_is_not_a_zero_endpoint_spec_16_3) {
    BEZEL_EXPECT_EQ(std::string("True\nTrue\nFalse\nFalse\nFalse\n"),
                    out("    var a: Range<int> := 1..;\n"
                        "    var b: Range<int> := 1..;\n"
                        "    var c: Range<int> := ..5;\n"
                        "    var d: Range<int> := ..5;\n"
                        "    var e: Range<int> := 0..5;\n"
                        "    var f: Range<int> := 1..0;\n"
                        "    Console.WriteLine(a.Equals(b));\n"
                        "    Console.WriteLine(c.Equals(d));\n"
                        "    Console.WriteLine(c.Equals(e));\n"
                        "    Console.WriteLine(a.Equals(f));\n"
                        "    Console.WriteLine(a.Equals(c));\n"
                        "    return 0;\n"));
}

// §8.4: "`a = b` invokes `a.Equals(b)`" — the two spellings agree on every
// pair, equal and unequal. Returns the index of the first disagreement.
BEZEL_TEST(range_equals_gethashcode_4126,
           equals_agrees_with_the_equality_operator_spec_8_4) {
    BEZEL_EXPECT_EQ(42, rc("    var r: Range<int> := 1..5;\n"
                           "    var a: Range<int> := 1..5;\n"
                           "    var b: Range<int> := 1..6;\n"
                           "    var c: Range<int> := 1..<5;\n"
                           "    var d: Range<int> := 1..5 step 2;\n"
                           "    var e: Range<int> := ..5;\n"
                           "    if r.Equals(a) <> (r = a) then return 1; end;\n"
                           "    if r.Equals(b) <> (r = b) then return 2; end;\n"
                           "    if r.Equals(c) <> (r = c) then return 3; end;\n"
                           "    if r.Equals(d) <> (r = d) then return 4; end;\n"
                           "    if r.Equals(e) <> (r = e) then return 5; end;\n"
                           "    if not r.Equals(a) then return 6; end;\n"
                           "    if r.Equals(b) then return 7; end;\n"
                           "    return 42;\n"));
}

// ── Receiver spellings: temporary, field, other element types ──────────────

BEZEL_TEST(range_equals_gethashcode_4126,
           temporary_receiver_and_temporary_argument_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nTrue\n"),
                    out("    Console.WriteLine((1..5).Equals(1..5));\n"
                        "    Console.WriteLine((1..5).Equals(1..<5));\n"
                        "    Console.WriteLine((1..5).GetHashCode() = "
                        "(1..5).GetHashCode());\n"
                        "    return 0;\n"));
}

BEZEL_TEST(range_equals_gethashcode_4126,
           field_receiver_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nTrue\n"),
                    out("    using var h := new Holder(3..9);\n"
                        "    var r: Range<int> := 3..9;\n"
                        "    Console.WriteLine(h.R.Equals(r));\n"
                        "    Console.WriteLine(h.R.Equals(3..8));\n"
                        "    Console.WriteLine(h.R.GetHashCode() = "
                        "r.GetHashCode());\n"
                        "    return 0;\n",
                        "class Holder\n"
                        "  public R: Range<int>;\n"
                        "  public constructor(r: Range<int>) R := r; end;\n"
                        "end Holder;\n"));
}

BEZEL_TEST(range_equals_gethashcode_4126,
           long_and_char_ranges_spec_16_3) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nTrue\nFalse\n"),
                    out("    var a: Range<long> := 1L..5000000000L;\n"
                        "    var b: Range<long> := 1L..5000000000L;\n"
                        "    var c: Range<long> := 1L..5000000001L;\n"
                        "    var x: Range<char> := 'a'..'e';\n"
                        "    var y: Range<char> := 'a'..'e';\n"
                        "    var z: Range<char> := 'a'..'f';\n"
                        "    Console.WriteLine(a.Equals(b));\n"
                        "    Console.WriteLine(a.Equals(c));\n"
                        "    Console.WriteLine(x.Equals(y));\n"
                        "    Console.WriteLine(x.Equals(z));\n"
                        "    return 0;\n"));
}

// ── GetHashCode ────────────────────────────────────────────────────────────

// §8.3: "if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`". The
// pre-fix answer was a constant 0 for every range, which satisfies that
// sentence vacuously — so the row also requires a nonzero hash and that
// ranges differing in one property hash apart.
BEZEL_TEST(range_equals_gethashcode_4126,
           equal_ranges_hash_equal_and_not_the_zero_degrade_spec_8_3) {
    BEZEL_EXPECT_EQ(42, rc("    var r1: Range<int> := 1..5;\n"
                           "    var r2: Range<int> := 1..5;\n"
                           "    var h1: int := r1.GetHashCode();\n"
                           "    var h2: int := r2.GetHashCode();\n"
                           "    if h1 <> h2 then return 1; end;\n"
                           "    if h1 = 0 then return 2; end;\n"
                           "    if (1..6).GetHashCode() = h1 then return 3; end;\n"
                           "    if (2..5).GetHashCode() = h1 then return 4; end;\n"
                           "    if (1..<5).GetHashCode() = h1 then return 5; end;\n"
                           "    if (1..5 step 2).GetHashCode() = h1 then "
                           "return 6; end;\n"
                           "    if (1..).GetHashCode() = (1..0).GetHashCode() "
                           "then return 7; end;\n"
                           "    return 42;\n"));
}

// §8.3: "The combining function must be order-sensitive ... a commutative
// combiner such as plain XOR does not satisfy this" — swapping Start and End
// must change the hash.
BEZEL_TEST(range_equals_gethashcode_4126,
           hash_combiner_is_order_sensitive_spec_8_3) {
    BEZEL_EXPECT_EQ(std::string("False\nFalse\n"),
                    out("    Console.WriteLine((1..5).GetHashCode() = "
                        "(5..1).GetHashCode());\n"
                        "    Console.WriteLine((3..7 step 2).GetHashCode() = "
                        "(7..3 step 2).GetHashCode());\n"
                        "    return 0;\n"));
}

// §8.6: a boxed range answers through the same generated members, so the box
// and the value must hash alike (one definition, not two that can drift).
BEZEL_TEST(range_equals_gethashcode_4126,
           value_hash_equals_boxed_hash_spec_8_3_spec_8_6) {
    BEZEL_EXPECT_EQ(std::string("True\nTrue\n"),
                    out("    var r: Range<int> := 1..5;\n"
                        "    var o: Object := r;\n"
                        "    Console.WriteLine(r.GetHashCode() = "
                        "o.GetHashCode());\n"
                        "    var s: Range<int> := ..<9;\n"
                        "    var p: Object := s;\n"
                        "    Console.WriteLine(s.GetHashCode() = "
                        "p.GetHashCode());\n"
                        "    return 0;\n"));
}

// ── The `Object` slot: non-own-typed arguments (§9.2, §8.1) ─────────────────

// §9.2: "any other argument compares unequal without the body running". An
// `Object` holding an equal `Range<int>` is equal; one
// holding a `Range<long>` with the same endpoints, a string, or an unequal
// range is not; `null` is not. The reverse direction (boxed receiver) agrees.
BEZEL_TEST(range_equals_gethashcode_4126,
           object_typed_argument_reaches_the_generated_equals_spec_9_2) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nFalse\nFalse\nFalse\nTrue\n"),
                    out("    var r: Range<int> := 1..5;\n"
                        "    var same: Object := 1..5;\n"
                        "    var wide: Object := 1L..5L;\n"
                        "    var text: Object := \"1..5\";\n"
                        "    var diff: Object := 1..6;\n"
                        "    var none: Object? := null;\n"
                        "    Console.WriteLine(r.Equals(same));\n"
                        "    Console.WriteLine(r.Equals(wide));\n"
                        "    Console.WriteLine(r.Equals(text));\n"
                        "    Console.WriteLine(r.Equals(diff));\n"
                        "    Console.WriteLine(r.Equals(none));\n"
                        "    Console.WriteLine(same.Equals(r));\n"
                        "    return 0;\n"));
}

BEZEL_TEST(range_equals_gethashcode_4126,
           nullable_range_argument_spec_8_3_spec_9_2) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nFalse\n"),
                    out("    var r: Range<int> := 1..5;\n"
                        "    var some: Range<int>? := 1..5;\n"
                        "    var other: Range<int>? := 1..6;\n"
                        "    var none: Range<int>? := null;\n"
                        "    Console.WriteLine(r.Equals(some));\n"
                        "    Console.WriteLine(r.Equals(other));\n"
                        "    Console.WriteLine(r.Equals(none));\n"
                        "    return 0;\n"));
}

BEZEL_TEST(range_equals_gethashcode_4126,
           other_range_instantiation_and_unrelated_argument_unequal_spec_9_2) {
    BEZEL_EXPECT_EQ(std::string("False\nFalse\n"),
                    out("    var r: Range<int> := 1..5;\n"
                        "    var w: Range<long> := 1L..5L;\n"
                        "    Console.WriteLine(r.Equals(w));\n"
                        "    Console.WriteLine(r.Equals(5));\n"
                        "    return 0;\n"));
}

// §22.5: the argument is evaluated exactly once whichever route answers —
// the own-typed comparison and the `Object`-slot one.
BEZEL_TEST(range_equals_gethashcode_4126,
           argument_evaluated_exactly_once_spec_22_5) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\n2\n"),
                    out("    var r: Range<int> := 1..5;\n"
                        "    Console.WriteLine(r.Equals(K.Next()));\n"
                        "    Console.WriteLine(r.Equals(K.Boxed()));\n"
                        "    Console.WriteLine(K.Calls);\n"
                        "    return 0;\n",
                        "class K\n"
                        "  public static Calls: int := 0;\n"
                        "  public static method Next(): Range<int>\n"
                        "    Calls := Calls + 1;\n"
                        "    return 1..5;\n"
                        "  end Next;\n"
                        "  public static method Boxed(): Object\n"
                        "    Calls := Calls + 1;\n"
                        "    return 1..6;\n"
                        "  end Boxed;\n"
                        "end K;\n"));
}

// ── Nullable receivers (§7.4 narrowing, §7.5 `?.`) and type parameters ─────

// §7.4: a narrowed `Range<int>?` receiver is the range itself; `!.` likewise.
BEZEL_TEST(range_equals_gethashcode_4126,
           narrowed_nullable_receiver_spec_7_4) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nTrue\nTrue\n"),
                    out("    var n: Range<int>? := 1..5;\n"
                        "    var r: Range<int> := 1..5;\n"
                        "    if n <> null then\n"
                        "      Console.WriteLine(n.Equals(r));\n"
                        "      Console.WriteLine(n.Equals(1..6));\n"
                        "      Console.WriteLine(n.GetHashCode() = "
                        "r.GetHashCode());\n"
                        "    end;\n"
                        "    Console.WriteLine(n!.Equals(r));\n"
                        "    return 0;\n"));
}

// §7.5: "`x?.M` returns null if `x` is null" and the member's answer
// otherwise. Before the fix both members on a `Range<T>?` receiver were a
// compile error ("has no lowering for the argument or receiver shape"), the
// fail-closed face of the same missing dispatch.
BEZEL_TEST(range_equals_gethashcode_4126,
           null_conditional_receiver_spec_7_5) {
    BEZEL_EXPECT_EQ(std::string("yes\nno\nnull\nTrue\nnull\n"),
                    out("    var some: Range<int>? := 1..5;\n"
                        "    var none: Range<int>? := null;\n"
                        "    var r: Range<int> := 1..5;\n"
                        "    var a: bool? := some?.Equals(r);\n"
                        "    var b: bool? := some?.Equals(1..6);\n"
                        "    var c: bool? := none?.Equals(r);\n"
                        "    Console.WriteLine(K.Show(a));\n"
                        "    Console.WriteLine(K.Show(b));\n"
                        "    Console.WriteLine(K.Show(c));\n"
                        "    var h: int? := some?.GetHashCode();\n"
                        "    var g: int? := none?.GetHashCode();\n"
                        "    Console.WriteLine(h = r.GetHashCode());\n"
                        "    if g = null then Console.WriteLine(\"null\"); "
                        "end;\n"
                        "    return 0;\n",
                        "class K\n"
                        "  public static method Show(v: bool?): string\n"
                        "    if v = null then return \"null\"; end;\n"
                        "    if v! then return \"yes\"; end;\n"
                        "    return \"no\";\n"
                        "  end Show;\n"
                        "end K;\n"));
}

// §9.2 *Equality through a type parameter*: a `T` bound to `Range<int>`
// reaches the same generated members, and a `Range<T>` built inside a generic
// method compares at its instantiation.
BEZEL_TEST(range_equals_gethashcode_4126,
           through_a_type_parameter_spec_9_2) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nTrue\nTrue\nFalse\n"),
                    out("    Console.WriteLine(G.Same<Range<int>>(1..5, 1..5));\n"
                        "    Console.WriteLine(G.Same<Range<int>>(1..5, 1..6));\n"
                        "    Console.WriteLine(G.Hash<Range<int>>(1..5) = "
                        "(1..5).GetHashCode());\n"
                        "    Console.WriteLine(G.Built<int>(1, 5, 5));\n"
                        "    Console.WriteLine(G.Built<int>(1, 5, 6));\n"
                        "    return 0;\n",
                        "class G\n"
                        "  public static method Same<T>(a: T, b: T): bool\n"
                        "    return a.Equals(b);\n"
                        "  end Same;\n"
                        "  public static method Hash<T>(a: T): int\n"
                        "    return a.GetHashCode();\n"
                        "  end Hash;\n"
                        "  public static method Built<T>(a: T, b: T, c: T): "
                        "bool\n"
                        "    var x: Range<T> := a..b;\n"
                        "    var y: Range<T> := a..c;\n"
                        "    return x.Equals(y);\n"
                        "  end Built;\n"
                        "end G;\n"));
}

// ── Sanitized leg: the `Object`-slot route mints and releases boxes ────────

// Looped so a per-call leak of the receiver's or the argument's box is not a
// one-shot the leak oracle could miss; the sum makes every iteration's answer
// observable (300 equal + 0 unequal). The argument shapes are a bound
// `Object` local, a range of another instantiation boxed at the call, and a
// nullable range. (An `Object` returned by a CALL as the argument is left
// out: that temporary leaks for every receiver kind — a record, a string, an
// `Object` — on the tree before this fix, a separate ownership defect.)
BEZEL_TEST(range_equals_gethashcode_4126,
           object_slot_route_is_clean_under_asan_spec_8_6) {
    auto r = runAotChecked(prog(
        "    var r: Range<int> := 1..5;\n"
        "    var n: int := 0;\n"
        "    for var i in 0..<300 do\n"
        "      var o: Object := 1..5;\n"
        "      if r.Equals(o) then n := n + 1; end;\n"
        "      if r.Equals(1L..5L) then n := n + 1000; end;\n"
        "      var q: Range<int>? := 1..5;\n"
        "      if not r.Equals(q) then n := n + 1000; end;\n"
        "      var b: bool? := q?.Equals(r);\n"
        "      if b = null then n := n + 1000; end;\n"
        "      var h: int? := q?.GetHashCode();\n"
        "      if h = null then n := n + 1000; end;\n"
        "    end;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_EQ(std::string("300\n"), r.stdoutText);
}

// ── Control: the user-record twin already answered this way ────────────────

BEZEL_TEST(range_equals_gethashcode_4126,
           control_user_body_record_with_the_same_properties_spec_8_3) {
    BEZEL_EXPECT_EQ(std::string("True\nFalse\nTrue\n"),
                    out("    var a := new Span4(1, 5, 1L, true);\n"
                        "    var b := new Span4(1, 5, 1L, true);\n"
                        "    var c := new Span4(1, 6, 1L, true);\n"
                        "    Console.WriteLine(a.Equals(b));\n"
                        "    Console.WriteLine(a.Equals(c));\n"
                        "    Console.WriteLine(a.GetHashCode() = "
                        "b.GetHashCode());\n"
                        "    return 0;\n",
                        "record Span4(Start: int?, End: int?, Step: long, "
                        "IsEndInclusive: bool);\n"));
}
