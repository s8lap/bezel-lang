// Spec-cited regression test for issue #8296 (§8.1 / §8.6 / §8.3, v0.355) —
// `Equals` with a VALUE-TYPED argument on a receiver of a no-override-identity
// builtin class (a Mutex lock handle, a `Stopwatch`, a `CancellationToken`)
// lowered to MALFORMED IR and aborted the compiler.
//
// ─── THE NORMATIVE SENTENCES (docs/Bezel_Language_Spec.md, v0.355) ─────────
//
// §8.1: `public class Object` declares
//     public virtual method Equals(other: Object?): bool;
// §8.6: "When a value type is used where `Object` (or an interface
//     implemented by the value type) is expected, the value is **boxed**: a
//     heap-allocated wrapper holds the value."
// §8.3: the default `Equals` of a CLASS is "Reference equality" — and it is
//     the default for a builtin class that declares no `Equals` override
//     (`Stopwatch` / `CancellationToken` / `Lock`: stdlib §6.1 / §6.2 and
//     §8.1, fix #4126).
// §8.4: `Object.ReferenceEquals` "compares object identity"; a value-typed
//     argument "produces a fresh box and compares unequal to every other
//     argument".
//
// ⇒ `l.Equals(5)` must BOX the `int` (§8.6) and compare the receiver with the
//   BOX (§8.3: reference equality) — the program COMPILES and the answer is
//   `False`.
//
// ─── THE DEFECT (MEASURED on a939f913) ─────────────────────────────────────
//
// The `bltn.eq.ref` arm of src/codegen/LlvmBackend_Calls2.cpp — the §8.3
// identity default for the no-override-identity builtin receivers — compared
// the receiver pointer with the RAW lowered argument, so a value-typed
// argument produced
//     %bltn.eq.ref = icmp eq ptr %3, i32 5
// — mismatched ICmp operand types — and the compiler ABORTED:
//     Both operands to ICmp instruction are not of the same type!
//     bezelc: codegen error: IR verification failed (malformed IR emitted;
//     see report above)
// MEASURED: `l.Equals(5)` on a Mutex lock handle and `sw.Equals(5)` on a
// `Stopwatch` both FAILED TO COMPILE; so did `l?.Equals(5)` / `sw?.Equals(5)`
// (the `?.` spelling of the same receivers falls through to the same arm) and
// every other value-typed argument shape (`bool`, `double`, `char`, `int?`,
// `()`).
//
// The REFERENCE-TYPED argument forms (`l.Equals(l2)`, `sw.Equals(null)`,
// `sw.Equals("x")`) compiled and printed `False` before the fix: they are
// the CONTROLS this fix must not disturb, and they are pinned below.
//
// The `Object`-receiver and user-class-receiver spellings of the same call
// (fix #3443's arm) already boxed the argument and are pinned too.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& imports, const std::string& body) {
    return "module M; " + imports +
           "public class Program public static method Main(): int " + body +
           "  return 0; end Main; end Program;";
}

// The §8.1 call on a Mutex lock handle (`Lock`, stdlib §6.2) — a builtin
// class with no `Equals` override.
std::string mutexProg(const std::string& body) {
    return prog("import System.Threading; ", body);
}

// The §8.1 call on a `Stopwatch` (stdlib §8.1) — a builtin class with no
// `Equals` override.
std::string swProg(const std::string& body) {
    return prog("import System.Diagnostics; ", body);
}

// `runAotOut` returns "<did-not-run>" when the program was REJECTED — which is
// exactly what the defect did (an IR-verification abort is a compile failure),
// so a rejected program fails these rows rather than passing them vacuously.
std::string out(const std::string& p) { return runAotOut(p); }

}  // namespace

// ─── THE ISSUE'S REPRO (RED without the fix) ───────────────────────────────

// #8296's repro, verbatim: `l.Equals(5)` on a Mutex lock handle. §8.6 boxes
// the `int`; §8.3's reference-equality default compares the receiver with the
// box → `False`. Before the fix this FAILED TO COMPILE ("Both operands to
// ICmp instruction are not of the same type!").
BEZEL_TEST(builtin_identity_equals_value_arg_8296, mutex_lock_handle_int) {
    BEZEL_EXPECT_EQ(out(mutexProg(
        "var m := new Mutex(); var l := m.Acquire(); "
        "Console.WriteLine(l.Equals(5)); "
        "l.Dispose(); m.Dispose(); ")),
        "False");
}

// The `Stopwatch` twin of the same repro.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_int) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(5)); ")),
        "False");
}

// The `?.` spelling of the same call on the same receivers (it reaches the
// same `bltn.eq.ref` arm). RED without the fix.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_int_null_conditional) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw: Stopwatch? := Stopwatch.StartNew(); "
        "Console.WriteLine(sw?.Equals(5) ?? false); ")),
        "False");
}

// ─── THE OTHER VALUE-TYPED ARGUMENT SHAPES (§8.6) ──────────────────────────

// `bool`, `double` and `char` are value types too: each is boxed (§8.6) and
// each compares unequal to the receiver (§8.3). RED without the fix.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_bool) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(true)); ")),
        "False");
}

BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_double) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(1.5)); ")),
        "False");
}

BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_char) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals('a')); ")),
        "False");
}

// A NON-CONSTANT argument: the row reads the data the fix handles (the
// argument's own value is lowered and boxed), so a fix that dropped or
// misread the argument would fail it. RED without the fix.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_int_local) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); var n: int := 5; "
        "Console.WriteLine(sw.Equals(n)); ")),
        "False");
}

// A NULLABLE value type (§8.6's "Nullable value-type boxing"): `int?` boxes to
// a boxed `int` when it holds a value, and to the null reference when it does
// not. RED without the fix.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_nullable_int) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); var n: int? := 5; "
        "Console.WriteLine(sw.Equals(n)); ")),
        "False");
}

// The UNIT value (§4.7 / §8.6's "Boxing of the unit value"): `()` is a value
// type and boxes like any other. RED without the fix.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_unit) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(())); ")),
        "False");
}

// ─── CONTROLS: the reference-typed argument forms (unchanged by the fix) ────

// Another lock handle: a reference-typed argument reaches the comparison
// UNBOXED, so the answer is the §8.3 identity of two distinct locks → `False`.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, mutex_lock_handle_lock) {
    BEZEL_EXPECT_EQ(out(mutexProg(
        "var m := new Mutex(); var l := m.Acquire(); "
        "var m2 := new Mutex(); var l2 := m2.Acquire(); "
        "Console.WriteLine(l.Equals(l2)); "
        "l.Dispose(); l2.Dispose(); m.Dispose(); m2.Dispose(); ")),
        "False");
}

// `null` is the null reference (§7) — never equal to the receiver.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_null) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(null)); ")),
        "False");
}

// A `string` is a reference type (§4.6): the bare pointer reaches the
// comparison, and a string is never the receiver → `False`.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_string) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(\"x\")); ")),
        "False");
}

// THE SAME RECEIVER: §8.3's reference-equality default is `True` for one
// reference compared with itself. This pins that the fix did not turn the
// comparison into something else.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_self) {
    BEZEL_EXPECT_EQ(out(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(sw)); ")),
        "True");
}

// ─── CONTROLS: the other §8.1 receivers (fix #3443's arm) ──────────────────

// An `Object`-typed receiver: the argument is boxed there too (§8.6) and the
// dispatch reaches the box's own `Equals` — the answer is `False` because
// the receiver holds a class instance, not an `int`.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, object_receiver_int) {
    BEZEL_EXPECT_EQ(out(prog(
        "public class Plain public V: int := 0; end Plain; ",
        "using var p := new Plain(); var o: Object := p; "
        "Console.WriteLine(o.Equals(5)); ")),
        "False");
}

// A user class receiver with no `Equals` override: §8.3's reference-equality
// default, with the argument boxed (§8.6) → `False`.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, class_receiver_int) {
    BEZEL_EXPECT_EQ(out(prog(
        "public class Plain public V: int := 0; end Plain; ",
        "using var p := new Plain(); "
        "Console.WriteLine(p.Equals(5)); ")),
        "False");
}

// ─── §28.9: the fresh argument box is discharged, not leaked ───────────────

// The box §8.6 mints for the argument is dead the instant the identity
// comparison is built (this arm compares two POINTERS and stores neither), so
// the producer's +1 is released — §28.9's runtime-managed rule. Under the
// ASan/LSan-instrumented oracle a leaked box (or a double free) flips the row.
BEZEL_TEST(builtin_identity_equals_value_arg_8296, stopwatch_int_no_leak) {
    auto r = runAotChecked(swProg(
        "using var sw := Stopwatch.StartNew(); "
        "Console.WriteLine(sw.Equals(5)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
