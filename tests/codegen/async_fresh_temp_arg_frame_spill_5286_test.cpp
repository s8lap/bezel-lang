// Regression tests for issue #5286 — §28.3 *Unbound owned temporaries* and
// *Deferred-activation callees*, §30.1, §30.13 (v0.313).
//
// A fresh owned class temporary passed as an argument (`new Item()`) is parked
// by buildOrderedArgs' #1019 arm (#993 for an `owned` formal) in an entry-block
// slot that its scope-end / unwind disposal later reads. Inside an `async`
// body, a real suspension between that store and the disposal means the
// disposal runs on the RESUMED invocation of `…$Resume`, where a plain entry
// alloca is uninitialised stack. The pre-fix result was SEGV,
// heap-buffer-overflow or a leak in `Main$Resume`. The fix promotes the slot
// into the state-machine frame (asyncHandlerSpillSlots_, the #2754 pattern).
//
// The awaited shape is legal by §28.3 *Deferred-activation callees*: an
// unbound owned temporary argument to an async callee is admissible exactly
// when the returned task settles within the same full expression, so the
// temporary is disposed after settlement, which is after the resume.
//
// Every row loops 200 times under runAotChecked (ASan + LSan; a report clears
// `ran`) and checks the destructor counter on stdout. `TakeA` returns
// 10 * (destructions during its own suspension) + x.V(), so an early dispose
// of the borrowed temporary shows up in `s` as well as in ASan.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kLib5286 = R"BZL(
public class Item
  public static n: int := 0;
  public constructor() end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method V(): int return 1; end V;
end Item;
public class Base
  public static hits: int := 0;
  public constructor() end constructor;
  public static method Hit() Base.hits := Base.hits + 1; end Hit;
  public async method TakeA(x: Item): Task<int>
    const before := Item.n;
    await Task.Delay(1);
    return 10 * (Item.n - before) + x.V();
  end TakeA;
  public async method TakeNoSusp(x: Item): Task<int>
    return x.V();
  end TakeNoSusp;
  public async method ThrowA(x: Item): Task<int>
    await Task.Delay(1);
    if x.V() = 1 then throw new InvalidOperationException("x"); end;
    return 0;
  end ThrowA;
  public method Two(x: Item, y: int): int return x.V() + y; end Two;
  public method TwoOwned(owned x: Item, y: int): int return x.V() + y; end TwoOwned;
  public static async method One(): Task<int>
    await Task.Delay(1);
    return 1;
  end One;
  public static async method Boom(): Task<int>
    await Task.Delay(1);
    throw new InvalidOperationException("b");
  end Boom;
end Base;
)BZL";

// `body` runs 200 times inside an async Main, with a fresh `b` per iteration.
std::string app5286(const std::string& body) {
    return std::string("module App5286;\n") + kLib5286 +
           "public class Program\n"
           "  public static async method Main(): Task<int>\n"
           "    var s := 0;\n"
           "    for var i in 0..<200 do\n"
           "      using var b := new Base();\n"
           "      " + body + "\n"
           "    end;\n"
           "    Console.WriteLine(\"n=\" + Item.n.ToString() + \" s=\" + "
           "s.ToString() + \" h=\" + Base.hits.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// The issue's headline: async borrow callee, fresh temporary, real suspension.
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           awaited_async_borrow_callee_fresh_temp_loop_200_spec_28_3) {
    auto r = runAotChecked(app5286("s := s + await b.TakeA(new Item());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=200 h=0\n"));
}

// The same shape on the plain (non-ASan) AOT oracle.
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           awaited_async_borrow_callee_fresh_temp_plain_aot_spec_28_3) {
    auto r = runAot(app5286("s := s + await b.TakeA(new Item());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=200 h=0\n"));
}

// The task faults after its suspension. The await rethrows on the resumed
// invocation, and the unwind edge disposes the temporary from the frame.
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           awaited_async_callee_faults_after_suspend_loop_200_spec_28_3) {
    auto r = runAotChecked(app5286(
        "try\n"
        "        s := s + await b.ThrowA(new Item());\n"
        "      catch\n"
        "        InvalidOperationException => Base.Hit(),\n"
        "      end try;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=0 h=200\n"));
}

// A SYNC borrow callee whose LATER argument awaits: the temporary is produced
// before the suspension and disposed after it.
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           sync_borrow_callee_later_arg_awaits_loop_200_spec_28_3) {
    auto r = runAotChecked(
        app5286("s := s + b.Two(new Item(), await Base.One()) - 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=200 h=0\n"));
}

// The later awaited argument faults: the borrow temporary is disposed on the
// unwind edge of the resumed invocation.
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           sync_borrow_callee_later_arg_faults_loop_200_spec_28_3) {
    auto r = runAotChecked(app5286(
        "try\n"
        "        s := s + b.Two(new Item(), await Base.Boom());\n"
        "      catch\n"
        "        InvalidOperationException => Base.Hit(),\n"
        "      end try;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=0 h=200\n"));
}

// An `owned` formal (#993 unwind-only entry): the later awaited argument
// faults, so the call never runs and the unwind edge must dispose the
// temporary from the frame.
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           owned_formal_later_arg_faults_loop_200_spec_28_3) {
    auto r = runAotChecked(app5286(
        "try\n"
        "        s := s + b.TwoOwned(new Item(), await Base.Boom());\n"
        "      catch\n"
        "        InvalidOperationException => Base.Hit(),\n"
        "      end try;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=0 h=200\n"));
}

// Control: the async callee never suspends, so the caller's await completes
// synchronously and the slot is read on the same invocation (clean pre-fix).
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           control_async_callee_without_suspension_loop_200_spec_28_3) {
    auto r = runAotChecked(app5286("s := s + await b.TakeNoSusp(new Item());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=200 h=0\n"));
}

// Control: the argument is a bound `using var`, not a temporary. The #1019 arm
// does not apply, and the local is frame-resident (clean pre-fix).
BEZEL_TEST(async_fresh_temp_arg_frame_spill_5286,
           control_bound_local_argument_loop_200_spec_28_3) {
    auto r = runAotChecked(app5286(
        "using var it := new Item();\n"
        "      s := s + await b.TakeA(it);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=200 s=200 h=0\n"));
}
