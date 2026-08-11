// fix #3044 — checked enum conversions (proposal 0078, graduated spec v0.242).
//
// §5.4 (v0.242): "An integer-to-enum cast `E(n)` is **checked against the
// declared constants**: it succeeds exactly when `n` equals the underlying
// value of some declared constant of `E`, and yields that value. … When `n` is
// not a constant, a value equal to no declared constant raises
// `OverflowException` at runtime, the diagnostic naming the enum type and the
// value. … **There is no value of an enum type that is not equal to a declared
// constant.**"
//
// Through v0.241 an in-range-but-undeclared `n` silently minted a "raw" enum
// value that no `case` arm could name — the one unchecked conversion in a
// checked language, and the reason §19.6's enum-exhaustiveness proof was
// unsound at runtime (rationale §9.12).
//
// This file pins the RUNTIME arm (the compile-time constant arm is the lit
// family `enum_cast_checked_3044__*`), the §10.4 / stdlib §9.9 branch tier
// `E.FromUnderlyingOrNull`, and the §21.7 statement-`case` totality grant the
// invariant unlocks.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// 0, 1, 2 by §10.1 auto-increment.
const char* kPriority = "module M; enum Priority Low, Medium, High; end Priority; ";
}  // namespace

// ---------------------------------------------------------------------------
// §5.4 — the runtime arm
// ---------------------------------------------------------------------------

// A non-constant `n` equal to no declared constant raises OverflowException.
// Was: silently produced a raw value and `int(p)` returned 7.
BEZEL_TEST(enum_checked_cast_3044, runtime_undeclared_raises_overflow_spec_5_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  try var n: int := 7; var p: Priority := Priority(n); return int(p); "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// OVER-REJECTION GUARD: a non-constant `n` that IS a declared value still
// yields that value — "it succeeds exactly when `n` equals the underlying value
// of some declared constant of `E`, and yields that value".
BEZEL_TEST(enum_checked_cast_3044, runtime_declared_value_succeeds_spec_5_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var n: int := 2; var p: Priority := Priority(n); return int(p); "
        "end Main; end C;"), 2);
}

// A SPARSE declared set (§10.2 explicit values): a value in a gap is undeclared
// even though it is well inside the underlying type's range, so the former #859
// range check said nothing about it.
BEZEL_TEST(enum_checked_cast_3044, runtime_explicit_value_gap_raises_spec_5_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum HttpStatus Ok := 200, Created := 201, NotFound := 404; "
        "end HttpStatus; "
        "class C public static method Main(): int "
        "  try var n: int := 300; var s: HttpStatus := HttpStatus(n); "
        "      return int(s); "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// Every declared value of a sparse set is accepted (the OR-chain arm of the
// membership test, as against the contiguous-range arm the other tests take).
BEZEL_TEST(enum_checked_cast_3044, runtime_sparse_set_all_values_accepted_spec_5_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum HttpStatus Ok := 20, Created := 21, NotFound := 44; "
        "end HttpStatus; "
        "class C public static method Main(): int "
        "  var a: int := 20; var b: int := 21; var c: int := 44; "
        "  return int(HttpStatus(a)) + int(HttpStatus(b)) + int(HttpStatus(c)); "
        "end Main; end C;"), 85);
}

// §10.2 duplicate constant values: "the declared-value set is the set of
// distinct values: `E(1)` where `Legacy := 1, Current := 1` yields that one
// value." Both aliases denote it, so the cast is accepted and compares equal to
// each of them.
BEZEL_TEST(enum_checked_cast_3044, runtime_duplicate_alias_value_accepted_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum E Legacy := 1, Current := 1; end E; "
        "class C public static method Main(): int "
        "  var n: int := 1; var e: E := E(n); "
        "  return int(e) + int(e = E.Legacy) + int(e = E.Current); "  // 1+1+1
        "end Main; end C;"), 3);
}

// §10.3 byte-backed enum: an in-underlying-range but undeclared value raises.
BEZEL_TEST(enum_checked_cast_3044, runtime_byte_backed_undeclared_raises_spec_5_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color: byte Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  try var n: int := 200; var c: Color := Color(n); return int(c); "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// §10.3 long-backed enum: the full 64-bit declared value round-trips (the
// membership test compares at the underlying width, not a truncated one).
BEZEL_TEST(enum_checked_cast_3044, runtime_long_backed_declared_ok_spec_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  var n: long := 9000000000L; var b: Big := Big(n); "
        "  return (if b = Big.B then 7 else 1); "
        "end Main; end C;"), 7);
}

// §10.3 long-backed enum: an undeclared 64-bit value raises — the check is over
// the declared VALUES, not over the underlying type's range (both fit `long`).
BEZEL_TEST(enum_checked_cast_3044, runtime_long_backed_undeclared_raises_spec_5_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Big: long A := 1L, B := 9000000000L; end Big; "
        "class C public static method Main(): int "
        "  try var n: long := 9000000001L; var b: Big := Big(n); return 1; "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// §10.2 negative declared values (a contiguous set whose low bound is negative)
// — the membership range compare must be exact across the sign boundary.
BEZEL_TEST(enum_checked_cast_3044, runtime_negative_declared_values_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Sign Neg := -1, Zero := 0, Pos := 1; end Sign; "
        "class C public static method Main(): int "
        "  var n: int := -1; var s: Sign := Sign(n); "
        "  var m: int := -2; "
        "  try var t: Sign := Sign(m); return 1; "
        "  catch OverflowException => return 40 + int(s = Sign.Neg) + 1; end; "
        "end Main; end C;"), 42);
}

// §5.1 "Nullable conversions" composed with §5.4: the cast is checked BEFORE
// the `Priority -> Priority?` wrap, so an undeclared value faults rather than
// arriving in the nullable slot as a present raw value. (The `E?(n)` spelling
// of §5.3 is a separate surface and is not exercised here.)
BEZEL_TEST(enum_checked_cast_3044, runtime_undeclared_into_nullable_slot_raises_spec_5_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C public static method Main(): int "
        "  try var n: int := 5; var p: Priority? := Priority(n); "
        "      return (if p = null then 1 else 2); "
        "  catch OverflowException => return 42; end; "
        "end Main; end C;"), 42);
}

// ---------------------------------------------------------------------------
// §5.4 — the fault's MESSAGE
// ---------------------------------------------------------------------------

// §5.4 (v0.242) is the one fault site in this area that specifies a message:
// the cast "raises `OverflowException` at runtime, **the diagnostic naming the
// enum type and the value**". (§6.1's checked arithmetic asks for none, which
// is why the shared guard-throw leaves the slot null.) Read it back off the
// caught exception.
BEZEL_TEST(enum_checked_cast_3044, runtime_fault_message_names_type_and_value_spec_5_4) {
    BEZEL_EXPECT_EQ(runAotOut(std::string(kPriority) +
        "class C public static method Main(): int "
        "  try var n: int := 7; var p: Priority := Priority(n); return int(p); "
        "  catch var e: OverflowException => Console.WriteLine(e.Message); end; "
        "  return 0; "
        "end Main; end C;"),
        std::string("enum cast: 7 is not the underlying value of any declared "
                    "constant of enum 'Priority'"));
}

// The same message on the NARROWING arm, which reports the SOURCE value (the
// one that did not fit) rather than the truncated one — `Color(300)` on a
// byte-backed enum must say 300, not 44.
BEZEL_TEST(enum_checked_cast_3044, runtime_narrowing_message_names_source_value_spec_5_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; enum Color: byte Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  try var n: int := 300; var c: Color := Color(n); return int(c); "
        "  catch var e: OverflowException => Console.WriteLine(e.Message); end; "
        "  return 0; "
        "end Main; end C;"),
        std::string("enum cast: 300 is not the underlying value of any declared "
                    "constant of enum 'Color'"));
}

// The message is built with a heap string handed to the exception's Message
// slot, so the fault path now allocates where it previously did not. Run the
// raise-and-catch under ASan/LSan to pin that the built-in disposer releases it
// — a leak here would be one per fault, on a path programs take under load.
BEZEL_TEST(enum_checked_cast_3044, runtime_fault_message_is_not_leaked_spec_5_4) {
    auto r = runAotChecked(std::string(kPriority) +
        "class C public static method Main(): int "
        "  var acc: int := 0; "
        "  for var i in 0..4 do "
        "    try var n: int := 7; var p: Priority := Priority(n); acc := acc + int(p); "
        "    catch OverflowException => acc := acc + 1; end; "
        "  end; "
        "  return acc; "                                          // 5
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
