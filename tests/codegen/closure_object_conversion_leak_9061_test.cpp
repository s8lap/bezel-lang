// Regression tests for #9061: a capture-free closure LOCAL converted to
// `Object` at a call argument (`reg.Keep(f)`) leaked its 64-byte closure cell
// (12800 B / 200 allocations over 200 loops), and so did a lambda literal at
// the same position and an `Object` local bound from the closure.
//
// WHICH REFERENCE WAS MISSING. Not the conversion's +1 and not the caller's
// disposal: the callee `Keep(o: Object) _o := o;` stores the `Object` view of
// the cell into an `Object?` FIELD, and that field held the cell WITHOUT a
// reference of its own and released nothing at disposal. The escape walk
// therefore had to call the argument an escape (the callee keeps it), so the
// caller never released its reference, and nobody ever did.
//
// THE RULE. §28.9 (v0.359): "Runtime management governs the *function value
// itself* -- the heap-allocated closure object that backs it"; §8.1: a
// function type reaches `Object` "via [its] reference-type heap
// representation". The slot that keeps the cell is therefore a holder of it
// and must hold a reference of its own (a run-time retain on a function value
// at every store into the field, a release of the displaced value and at the
// holder's disposal); the caller then keeps and releases its own at scope end.
//
// The rows run 200 loops on `runAotChecked` (ASan + leak check) with exact
// stdout, so they are RED on the base tree by the leak and also pin that the
// kept closure stays callable after the local's scope ends and after a second
// store (no use-after-free, no double release).
//
// per §28.9, §8.1, §27.5 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
template <class R>
std::string outOf(const R& r) {
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

std::string program(const std::string& body) {
    return std::string(R"BZL(
module M;
public class Reg2
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method KeepN(o: Object?) _o := o; end KeepN;
  public method Clear() _o := null; end Clear;
  public method Run(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end Run;
end Reg2;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
)BZL") + body + R"BZL(
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
)BZL";
}
}  // namespace

BEZEL_TEST(closure_object_conversion_leak_9061, issue_repro_local_closure_at_object_arg_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.Keep(f);
    t := t + 1;
)BZL"))), "200\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, lambda_literal_at_object_arg_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    reg.Keep(lambda() => 9);
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, object_local_at_object_arg_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    var o: Object := f;
    reg.Keep(o);
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, stored_closure_invoked_after_keep_and_local_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.Keep(f);
    t := t + f() + reg.Run();
)BZL"))), "3600\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, kept_twice_and_overwritten_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    var g: () -> int := lambda() => 4;
    reg.Keep(f);
    reg.Keep(f);
    reg.Keep(g);
    t := t + reg.Run();
)BZL"))), "800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, nullable_object_param_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.KeepN(f);
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, local_dies_before_holder_still_callable_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    if true then
      var f: () -> int := lambda() => 9;
      reg.Keep(f);
    end if;
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, one_closure_in_two_holders_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var r1 := new Reg2();
    var r2 := new Reg2();
    var f: () -> int := lambda() => 9;
    r1.Keep(f);
    r2.Keep(f);
    t := t + r1.Run() + r2.Run();
)BZL"))), "3600\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, cleared_holder_releases_its_reference_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.Keep(f);
    reg.Clear();
    t := t + reg.Run();
)BZL"))), "1400\n");
}
