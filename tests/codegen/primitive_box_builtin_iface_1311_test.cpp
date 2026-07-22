// Spec-derived regression tests for #1311: a PRIMITIVE value type boxes into a
// builtin GENERIC interface it implements — IComparable<T>/IEquatable<T>
// (stdlib §9.1–§9.5) — per §8.6 ("When a value type is used where Object (or an
// interface implemented by the value type) is expected, the value is boxed")
// and the §5.1 implicit-conversion rule that admits that boxing.
//
// Before the fix the boxing was wrongly rejected at type-check as "not
// implicitly convertible (§5.1)" — `var c: IComparable<int> := 5` did not
// compile. Distinct from #1115 (a user CLASS conforming to IComparable<int> as a
// value — that path already worked): #1311 is the compiler-intrinsic primitive
// itself (int/long/double/char/bool/…) being boxed into the interface, which
// required (a) the §5.1 boxing check to recognise the primitive implements the
// builtin interface and (b) the primitive box vtable to carry a real itable so
// virtual dispatch on the interface reaches a CompareTo/Equals (previously the
// box's itmap was null, so itable lookup crashed).
//
// Positive cases use runJit (returns the FULL int from Main()); negative cases
// assert the front end rejects the invalid boxing (§5.1 — no widening: Byte
// implements IComparable<byte>, not IComparable<int>; bool implements
// IEquatable<bool> only, §9.4 — no IComparable<bool>).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
bool compiles(const std::string& program) {
    return runAot(program).ran;
}
}  // namespace

// #1311 / §8.6 / §5.1: the literal repro — an int boxes into IComparable<int>
// and CompareTo dispatches through the interface (the boxed int's itable).
// Without the sema fix this did not compile; without the codegen fix the
// itable lookup crashed. 5.CompareTo(3) > 0 → 100.
BEZEL_TEST(primitive_box_builtin_iface_1311, int_boxes_into_icomparable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var c: IComparable<int> := 5; "
        "  return (if c.CompareTo(3) > 0 then 100 else 200); "
        "end Main; end C;"), 100);
}

// #1311: int boxes into IEquatable<int>; Equals dispatches true for an equal
// argument (e.Equals(7)→true, contributes 10) and false for a non-equal one
// (e.Equals(9)→false, contributes 1 via the ELSE branch). Both terms are
// load-bearing: the second is non-zero ONLY when Equals correctly rejects an
// unequal value, so an always-true or always-false thunk fails this test.
BEZEL_TEST(primitive_box_builtin_iface_1311, int_boxes_into_iequatable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var e: IEquatable<int> := 7; "
        "  return (if e.Equals(7) then 10 else 0) + (if e.Equals(9) then 0 else 1); "
        "end Main; end C;"), 11);
}

// #1311: a WIDE primitive (long/i64) boxes into IComparable<long> and
// dispatches — the itable thunk's i64 ABI matches the arg (the call site's
// concrete-fnTy reconciliation keeps the call well-typed).
BEZEL_TEST(primitive_box_builtin_iface_1311, long_boxes_into_icomparable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var l: IComparable<long> := 10L; "
        "  return (if l.CompareTo(3L) > 0 then 5 else 0) + l.CompareTo(10L); "
        "end Main; end C;"), 5);
}

// #1311: char and double box into their IComparable<T> (signed/ordered-FP
// comparison semantics in the itable thunk match the direct lowering).
BEZEL_TEST(primitive_box_builtin_iface_1311, char_and_double_box_into_icomparable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var ch: IComparable<char> := 'b'; "
        "  var d: IComparable<double> := 2.5; "
        "  return (if ch.CompareTo('a') > 0 then 1 else 0) "
        "       + (if d.CompareTo(3.0) < 0 then 1 else 0); "
        "end Main; end C;"), 2);
}

// #1311 / §9.4: bool implements IEquatable<bool> but NOT IComparable<bool>; the
// IEquatable<bool> boxing + Equals dispatch must work. b.Equals(true)→true
// (contributes 8); b.Equals(false)→false, verified via the second term being
// non-zero ONLY when Equals correctly rejects an unequal value (an always-true
// broken thunk would instead yield 8 + 9 = 17, failing this test).
BEZEL_TEST(primitive_box_builtin_iface_1311, bool_boxes_into_iequatable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var b: IEquatable<bool> := true; "
        "  return (if b.Equals(true) then 8 else 0) + (if b.Equals(false) then 9 else 0); "
        "end Main; end C;"), 8);
}

// #1311 control: non-generic boxing (int → Object) still compiles and runs —
// the change to the primitive box vtable must not regress the Object path.
BEZEL_TEST(primitive_box_builtin_iface_1311, object_boxing_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var o: Object := 5; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// #1311 control: a user CLASS conforming to IComparable<int> (the #1115 path)
// still dispatches through its itable — the primitive-box itable addition must
// not disturb the user-class itable machinery.
BEZEL_TEST(primitive_box_builtin_iface_1311, user_class_icomparable_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class UC: IComparable<int> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method CompareTo(other: int): int => (if _v > other then 2 else 0); "
        "end UC; "
        "class C public static method Main(): int "
        "  using var u := new UC(5); var x: IComparable<int> := u; "
        "  return x.CompareTo(3); "  // 2
        "end Main; end C;"), 2);
}

// ── §5.1 negative controls: an INVALID boxing is still rejected. No widening
// (Byte/Int32 implement IComparable<byte>/IComparable<int>, their own alias),
// and bool implements no IComparable at all (§9.4). These must NOT compile.

// int does NOT implement IComparable<long> → rejected.
BEZEL_TEST(primitive_box_builtin_iface_1311, int_to_icomparable_long_rejected) {
    BEZEL_EXPECT_FALSE(compiles(
        "module M; "
        "class C public static method Main(): int "
        "  var c: IComparable<long> := 5; return 0; "
        "end Main; end C;"));
}

// bool does NOT implement IComparable<bool> (§9.4) → rejected.
BEZEL_TEST(primitive_box_builtin_iface_1311, bool_to_icomparable_bool_rejected) {
    BEZEL_EXPECT_FALSE(compiles(
        "module M; "
        "class C public static method Main(): int "
        "  var c: IComparable<bool> := true; return 0; "
        "end Main; end C;"));
}

// int does NOT implement IEquatable<string> → rejected.
BEZEL_TEST(primitive_box_builtin_iface_1311, int_to_iequatable_string_rejected) {
    BEZEL_EXPECT_FALSE(compiles(
        "module M; "
        "class C public static method Main(): int "
        "  var e: IEquatable<string> := 5; return 0; "
        "end Main; end C;"));
}
