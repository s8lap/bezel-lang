// Regression tests for issue #5292 - §28.3 *Unbound owned temporaries* rule 1
// and *Deferred-activation callees*, §30.1 (v0.313).
//
// buildOrderedArgs parks several per-argument disposal slots in plain
// entry-block allocas: the fresh IEnumerable<T> wrapper of an Array<T> /
// LinkedList<T> / OwningArray<T> argument (`wrapMade` arm, scope-end
// iterEnumDispose, #1923 / #2298 / #1069), the temporary of a collection
// literal passed to a borrow parameter, and the scratch of an `out _` against
// an `out owned` class parameter. Inside an `async` body a LATER argument that
// awaits puts a real suspension between the store and the disposal, so the
// disposal runs on the RESUMED `...$Resume` invocation where the entry alloca is
// uninitialised stack (pre-fix: SEGV in Main$Resume, or in bz_array_free for
// the literal). The fix promotes each slot into the state-machine frame
// (asyncHandlerSpillSlots_, the #2754 / #5286 pattern).
//
// The programs are legal by §28.3 *Unbound owned temporaries* rule 1: the
// wrapper is disposed at the end of the full expression, i.e. after the await.
//
// Every row loops 200 times under runAotChecked (ASan + LSan; a report clears
// `ran`) and checks stdout exactly. `n` counts OwningArray<Item> element
// destructions (2 per iteration => 400 exactly once each), `s` the callee
// results, `h` the caught faults.
//
// Imported-callee rows do not exist: an IEnumerable<T> argument to an IMPORTED
// callee is not wrapped at all (a separate defect, reported on #5292), so the
// eslot arm is never reached there.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kLib5292 = R"BZL(
public class Item
  public static n: int := 0;
  public constructor() end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method V(): int return 1; end V;
end Item;
public class Base
  public static hits: int := 0;
  public constructor() end constructor;
  public method Sum(xs: IEnumerable<int>, k: int): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t + k;
  end Sum;
  public method SumFirst(k: int, xs: IEnumerable<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t + k;
  end SumFirst;
  public method SumA(xs: Array<int>, k: int): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t + k;
  end SumA;
  public method SumO(xs: IEnumerable<Item>, k: int): int
    var t := 0;
    for var x in xs do t := t + x.V(); end;
    return t + k;
  end SumO;
  public method MkOut(out owned r: Item, k: int): int
    r := new Item();
    return k;
  end MkOut;
  public static async method P(): Task<int>
    await Task.Delay(0);
    return 1;
  end P;
  public static async method BoomA(): Task<int>
    await Task.Delay(0);
    throw new InvalidOperationException("b");
  end BoomA;
  public static method Boom(): int throw new InvalidOperationException("x"); end Boom;
  public static method Hit() Base.hits := Base.hits + 1; end Hit;
end Base;
)BZL";

// `stmt` runs 200 times inside an async Main under `try ... catch`, with a
// fresh `b`, an Array<int> `arr`, a LinkedList<int> `lst` and an
// OwningArray<Item> `oa` per iteration.
std::string app5292(const std::string& stmt, bool isAsync = true) {
    return std::string("module App5292;\n") + kLib5292 +
           "public class Program\n"
           "  public static " + (isAsync ? "async method Main(): Task<int>\n"
                                          : "method Main(): int\n") +
           "    var s := 0;\n"
           "    for var i in 0..<200 do\n"
           "      using var b := new Base();\n"
           "      using var arr := new Array<int>(3);\n"
           "      arr.Add(1); arr.Add(2); arr.Add(3);\n"
           "      using var lst := new LinkedList<int>();\n"
           "      lst.Add(1); lst.Add(2); lst.Add(3);\n"
           "      using var oa := new OwningArray<Item>(2);\n"
           "      oa.Add(new Item()); oa.Add(new Item());\n"
           "      try\n"
           "        " + stmt + "\n"
           "      catch\n"
           "        InvalidOperationException => Base.Hit(),\n"
           "      end try;\n"
           "    end;\n"
           "    Console.WriteLine(\"n=\" + Item.n.ToString() + \" s=\" + "
           "s.ToString() + \" h=\" + Base.hits.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// The issue's repro: Array<int> as IEnumerable<int>, later argument awaits.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           array_wrapper_later_arg_awaits_loop_200_spec_28_3) {
    auto r = runAotChecked(app5292("s := s + b.Sum(arr, await Base.P());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=1400 h=0\n"));
}

// The same shape on the plain (non-ASan) AOT oracle.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           array_wrapper_later_arg_awaits_plain_aot_spec_28_3) {
    auto r = runAot(app5292("s := s + b.Sum(arr, await Base.P());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=1400 h=0\n"));
}

// A LinkedList<int> source takes the same wrapMade arm.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           linkedlist_wrapper_later_arg_awaits_loop_200_spec_28_3) {
    auto r = runAotChecked(app5292("s := s + b.Sum(lst, await Base.P());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=1400 h=0\n"));
}

// An OwningArray<Item> source (borrowing wrap; the elements are disposed once,
// by the owner, not by the wrapper).
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           owningarray_wrapper_later_arg_awaits_loop_200_spec_28_3) {
    auto r = runAotChecked(app5292("s := s + b.SumO(oa, await Base.P());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=600 h=0\n"));
}

// The later awaited argument FAULTS after its suspension: the wrapper is
// disposed exactly once, on the unwind edge of the resumed invocation.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           array_wrapper_later_arg_faults_after_await_loop_200_spec_28_3) {
    auto r = runAotChecked(
        app5292("s := s + b.Sum(arr, await Base.BoomA());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=0 h=200\n"));
}

// Collection literal passed to a borrow parameter, later argument awaits: the
// literal temporary's disposal slot is the second slot of the same family.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           collection_literal_temp_later_arg_awaits_loop_200_spec_28_3) {
    auto r = runAotChecked(
        app5292("s := s + b.SumA([1, 2, 3], await Base.P());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=1400 h=0\n"));
}

// `out _` against `out owned` with a later awaited argument that faults: the
// scratch is disposed (null-guarded, exactly once) from the frame. Clean
// pre-fix by luck of stack contents; pinned as a guard for the third slot.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           out_discard_scratch_later_arg_faults_loop_200_spec_28_3) {
    auto r = runAotChecked(
        app5292("s := s + b.MkOut(out _, await Base.BoomA());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=0 h=200\n"));
}

// Control: the await is in an EARLIER argument, so no suspension sits between
// the wrapper's store and its disposal (clean pre-fix).
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           control_await_in_earlier_argument_loop_200_spec_28_3) {
    auto r = runAotChecked(
        app5292("s := s + b.SumFirst(await Base.P(), arr);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=1400 h=0\n"));
}

// Control: a faulting SYNC later argument (no suspension; clean pre-fix).
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           control_later_arg_sync_fault_loop_200_spec_28_3) {
    auto r = runAotChecked(app5292("s := s + b.Sum(arr, Base.Boom());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=0 h=200\n"));
}

// Control: async body without any suspension in the statement.
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           control_async_body_no_await_loop_200_spec_28_3) {
    auto r = runAotChecked(app5292("s := s + b.Sum(arr, 1);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=1400 h=0\n"));
}

// Control: a SYNC body (no state machine, the plain entry alloca stays).
BEZEL_TEST(async_iterenum_arg_slot_frame_spill_5292,
           control_sync_body_loop_200_spec_28_3) {
    auto r = runAotChecked(
        app5292("s := s + b.Sum(arr, 1) + b.SumA([1, 2, 3], 1) + "
                "b.SumO(oa, 1);",
                /*isAsync=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=400 s=3400 h=0\n"));
}
