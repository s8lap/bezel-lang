// Regression tests for #8843: a lambda LITERAL passed directly as an argument
// to a user method's `Object` parameter is never released (64 B per call,
// capturing or not). Binding it to a local first was already clean.
//
// The capturing form (`var n := i; Take(lambda() => n)`) is rejected on this
// tree ("carries captured borrow(s) ... declared Object"); it becomes reachable
// with #8785's set-erasing admission and is not pinned here.
//
// Spec (v0.359):
//  - §28.9: runtime-managed values (function values among them) are released
//    when their last owner ends; the temporary has no owner after the call,
//    so the caller's reference is the one to drop;
//  - §8.1: a function type reaches `Object` "via [its] reference-type heap
//    representation", so the callee receives the very cell;
//
// The release is armed only where the callee body provably only null-tests /
// type-tests the formal (the closure-LOCAL arm of #8743, applied to the
// literal). A callee that STORES the formal keeps today's bounded leak (the
// safe direction): the store row runs under `runAotCheckedLenient` (ASan on,
// LSan off) and pins that no use-after-free appears.
//
// per §28.9, §8.1 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
template <class R>
std::string outOf(const R& r) {
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
}  // namespace

// The issue's repro, as filed (capture-free literal, 200 calls).
BEZEL_TEST(object_formal_lambda_literal_8843,
           capture_free_literal_at_object_formal_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(R"BZL(
module M;
public class Program
  public static method Take(o: Object) end Take;
  public static method Main(): int
    for var i in 0 ..< 200 do Take(lambda() => 5); end for;
    Console.WriteLine("ran"); return 0;
  end Main;
end Program;
)BZL")), "ran\n");
}

// The callee inspects the formal (null test and type test): still released
// once, after the call.
BEZEL_TEST(object_formal_lambda_literal_8843,
           inspecting_callee_literal_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(R"BZL(
module M;
public class Program
  public static method Probe(o: Object?): int
    if o = null then return -1; end;
    if o is string then return 1; end;
    return 2;
  end Probe;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do t := t + Probe(lambda() => 7); end for;
    Console.WriteLine(t); return 0;
  end Main;
end Program;
)BZL")), "400\n");
}

// UAF guard: the callee STORES the Object. The release must not fire there
// (a capture-free literal, the only kind a set-erasing position admits for
// the storing shape); the stored cell is still readable afterwards.
BEZEL_TEST(object_formal_lambda_literal_8843,
           storing_callee_literal_not_released_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotCheckedLenient(R"BZL(
module M;
public class Holder
  public Keep: Object?;
  public constructor() end constructor;
  public method Store(o: Object) this.Keep := o; end Store;
end Holder;
public class Program
  public static method Main(): int
    var h := new Holder();
    for var i in 0 ..< 50 do h.Store(lambda() => 6); end for;
    Console.WriteLine(h.Keep <> null);
    return 0;
  end Main;
end Program;
)BZL")), "True\n");
}
