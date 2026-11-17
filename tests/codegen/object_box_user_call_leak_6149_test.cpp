// An `Object` box local passed to a user method is never released
// (§8.6 / §14.1 / §28.3 / §28.9, v0.357; #6149).
//
// §8.6: converting a value to `Object` allocates a box the binding owns. The
// box local's scope-end release (`collectBoxReleaseEligible`) arms only when
// the box escape walk proves no reference to the box outlives the local. A
// plain argument to a user method was always read as an ESCAPE, even when
// the callee's own body provably does not let its `Object` parameter outlive
// the call (the #2952 summary, `isCalleeParamNonEscaping`, already used for
// string and record arguments). The box leaked once per call. For a GENERIC
// callee the summary is now taken for the call's instantiation: the callee's
// type parameters are bound from the explicit type arguments, or else from
// the arguments whose formal is a bare type parameter.
//
// MEASURED (bezelc --sanitize=address, r30-c1 4d689965e): every program below
// leaks one box per iteration on the base (LeakSanitizer); with the fix each
// is clean. Every program loops 100x (#8630: a single leaked object can hide
// behind a stale stack word).
//
// The twins in which the callee STORES the box (a plain and a generic callee,
// a box over a record with a runtime string, a box over a string) and the
// generic `Same<T>(x, o) => x.Equals(o)` over a class whose `Equals`
// override stores `o` were probed 200x under ASan on both trees: none frees
// early (the lane handback lists them).
//
// Not covered: the temporary boxed into an INTERFACE-DISPATCHED `Object?`
// parameter (#6149's second shape). The body that runs is chosen at run time
// among implementations the summary cannot enumerate soundly, so the
// argument stays an escape (narrow #6149).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
void expectCleanRun(const std::string& program, const std::string& expected) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

std::string loopProgram(const std::string& helpers, const std::string& once) {
    return "module M; record Name(value: string, n: int); "
           "class C " +
           helpers +
           "  static method Once(i: int): int " + once +
           "  end Once; "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<100 do t := t + Once(i); end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end C;";
}
}  // namespace

// §8.6 / §28.3: an `int` boxed into an `Object` local, passed to a method
// that only reads its parameter.
BEZEL_TEST(object_box_user_call_leak_6149,
           int_box_local_to_reading_callee_released_spec_8_6_s28_3) {
    expectCleanRun(loopProgram("  static method ReadO(o: Object): int "
                               "    => (if o <> null then 1 else 0); ",
                               "var o: Object := i; "
                               "return ReadO(o);"),
                   "100\n");
}

// §8.6 / §9.1: a record with a runtime-built string boxed into an `Object`
// local, passed to the same kind of callee — the box and its string.
BEZEL_TEST(object_box_user_call_leak_6149,
           record_box_local_to_reading_callee_released_spec_8_6_s28_3) {
    expectCleanRun(loopProgram("  static method ReadO(o: Object): int "
                               "    => (if o <> null then 1 else 0); ",
                               "var t := \"x\" + i.ToString(); "
                               "var o: Object := new Name(t, 1); "
                               "return ReadO(o);"),
                   "100\n");
}

// §14.1 / §28.3: a GENERIC callee, instantiated by an explicit type argument
// and by inference from a bare-type-parameter argument.
BEZEL_TEST(object_box_user_call_leak_6149,
           box_local_to_generic_reading_callee_released_spec_14_1_s28_3) {
    expectCleanRun(loopProgram("  static method ReadG<T>(x: T, o: Object): int "
                               "    => (if o <> null then 1 else 0); ",
                               "var o: Object := i; "
                               "return ReadG<int>(i, o) + ReadG(i, o) - 1;"),
                   "100\n");
}

// §9.2 / §14.1: the #5924 shape — `Same<T>(x: T, o: Object?) => x.Equals(o)`
// at a value record whose DECLARED `Equals(other: Money)` reads only a
// scalar. At T = Money the receiver is a value, so the call cannot store
// `o`; the walk sees that only for the instantiation (an unbound T could be
// a class whose `Equals` override keeps `o`). Explicit and inferred T.
BEZEL_TEST(object_box_user_call_leak_6149,
           box_local_to_generic_declared_equals_released_spec_9_2_s14_1) {
    auto r = runAotChecked(
        "module M; "
        "public record Money "
        "  public readonly property Amount: int; "
        "  public constructor(amount: int) "
        "    Amount := amount; "
        "  end constructor; "
        "  public method Equals(other: Money): bool => Amount = other.Amount; "
        "  public method GetHashCode(): int => Amount; "
        "end Money; "
        "class C "
        "  static method Same<T>(x: T, o: Object?): bool where T: notnull "
        "    => x.Equals(o); "
        "  static method Once(i: int): int "
        "    var a := new Money(1); "
        "    var o: Object? := new Money(i); "
        "    var n := 0; "
        "    if Same<Money>(a, o) then n := n + 1; end; "
        "    if Same(a, o) then n := n + 1; end; "
        "    return 1 + n; "
        "  end Once; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do t := t + Once(i); end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("102\n"));
}
