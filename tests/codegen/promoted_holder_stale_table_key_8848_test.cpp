// Codegen — issue #8848 (§27.5 Closures, §21.4 The `for` statement, §30.2
// `await`, §30.13 Runtime model, spec v0.359): a capture-cell HOLDER that an
// async body's suspension moves into the frame's spill area must not leave
// anything behind that a LATER function's stack slot can be mistaken for.
//
// Mechanism: promoteDemotedSlotsToFrame replaces every spilled alloca of a
// resume function with a pointer into the calloc'd `$Resume.spill` struct and
// takes the alloca out of the function. It used to FREE that AllocaInst while
// the emitter's address-keyed side tables still named it (`cellVars_`: "this
// slot holds a pointer to a capture cell of element type T", never cleared
// between methods). The allocator then gave the same address to an alloca of a
// later function, and that function read its own plain `i32` local through
// the stale entry as a cell holder: an 8-byte read of a 4-byte stack slot
// (ASan stack-buffer-overflow in the CALLER's resume function, which captures
// nothing), or IR that does not verify.
//
// Every row loops capture-then-`await` over a collection, which since #6953
// gives each captured `for`-variable an eagerly boxed holder with a spill-area
// home, so one async method retires several holders at once. Each row is red
// without the fix (measured: stack-buffer-overflow in
// `bz$async$M.C$Main$Resume` or "IR verification failed") and uses
// runAotChecked: generated-code ASan plus LeakSanitizer, so the cells freed
// per iteration are checked as well.
// per §27.5, §21.4, §30.2, §30.13 (issue #8848)

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// The issue's repro: an async method with SIX `for var yN in xs` loops, each
// capturing its loop variable and awaiting before it calls the lambda, called
// 200 times from an async Main that captures nothing.
// per §27.5 ("each iteration produces a fresh binding"), §30.2 (issue #8848)
BEZEL_TEST(promoted_holder_stale_table_key_8848,
           six_captured_for_in_loops_then_async_main_x200_spec_27_5_30_2) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(module M;
class C
  public static async method Sum(xs: Array<int>): Task<int>
    var t := 0;
    for var y0 in xs do
      var f0 := lambda(): int => y0;
      await Task.Delay(0);
      t := t + f0();
    end;
    for var y1 in xs do
      var f1 := lambda(): int => y1;
      await Task.Delay(0);
      t := t + f1();
    end;
    for var y2 in xs do
      var f2 := lambda(): int => y2;
      await Task.Delay(0);
      t := t + f2();
    end;
    for var y3 in xs do
      var f3 := lambda(): int => y3;
      await Task.Delay(0);
      t := t + f3();
    end;
    for var y4 in xs do
      var f4 := lambda(): int => y4;
      await Task.Delay(0);
      t := t + f4();
    end;
    for var y5 in xs do
      var f5 := lambda(): int => y5;
      await Task.Delay(0);
      t := t + f5();
    end;
    return t;
  end Sum;
  public static async method Main(): Task<int>
    var xs: Array<int> := [1, 2, 3];
    var a := 0;
    for var k in 0..<200 do a := a + await Sum(xs); end;
    Console.WriteLine(a);
    return 0;
  end Main;
end C;)BZL"),
        "7200\n");
}

// The same with EIGHT loops: more retired holders, so a different later slot
// lands on a retired address.
// per §27.5, §30.2 (issue #8848)
BEZEL_TEST(promoted_holder_stale_table_key_8848,
           eight_captured_for_in_loops_then_async_main_x200_spec_27_5_30_2) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(module M;
class C
  public static async method Sum(xs: Array<int>): Task<int>
    var t := 0;
    for var y0 in xs do
      var f0 := lambda(): int => y0;
      await Task.Delay(0);
      t := t + f0();
    end;
    for var y1 in xs do
      var f1 := lambda(): int => y1;
      await Task.Delay(0);
      t := t + f1();
    end;
    for var y2 in xs do
      var f2 := lambda(): int => y2;
      await Task.Delay(0);
      t := t + f2();
    end;
    for var y3 in xs do
      var f3 := lambda(): int => y3;
      await Task.Delay(0);
      t := t + f3();
    end;
    for var y4 in xs do
      var f4 := lambda(): int => y4;
      await Task.Delay(0);
      t := t + f4();
    end;
    for var y5 in xs do
      var f5 := lambda(): int => y5;
      await Task.Delay(0);
      t := t + f5();
    end;
    for var y6 in xs do
      var f6 := lambda(): int => y6;
      await Task.Delay(0);
      t := t + f6();
    end;
    for var y7 in xs do
      var f7 := lambda(): int => y7;
      await Task.Delay(0);
      t := t + f7();
    end;
    return t;
  end Sum;
  public static async method Main(): Task<int>
    var xs: Array<int> := [1, 2, 3];
    var a := 0;
    for var k in 0..<200 do a := a + await Sum(xs); end;
    Console.WriteLine(a);
    return 0;
  end Main;
end C;)BZL"),
        "9600\n");
}

// Four captured loops over four kinds of source (LinkedList<int>, string, an
// iterator method's IEnumerable<int>, Array<Item>) in an async method, and the
// same four in a sync iterator, both driven 200 times from an async Main.
// Without the fix the module does not verify ("IR verification failed"): the
// stale entry makes a later function address another function's slot.
// per §27.5, §21.4, §30.2 (issue #8848)
BEZEL_TEST(promoted_holder_stale_table_key_8848,
           four_source_kinds_async_method_and_sync_iterator_x200_spec_27_5_21_4) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(module M;
class Item
  public V: int;
  public constructor(v: int)
    V := v;
  end;
end Item;
class C
  public static method Src(n: int): IEnumerable<int>
    for var i in 0..<n do yield return i; end;
  end Src;
  public static async method Sum(ll: LinkedList<int>, s: string, items: Array<Item>): Task<int>
    var t := 0;
    for var b in ll do
      var f := lambda(): int => b * 2;
      await Task.Delay(0);
      t := t + f();
    end;
    for var ch in s do
      var f := lambda(): int => ch.ToString().Length;
      await Task.Delay(0);
      t := t + f();
    end;
    for var e in Src(10) do
      var f := lambda(): int => e * 3;
      await Task.Delay(0);
      t := t + f();
    end;
    for var it in items do
      var f := lambda(): int => it.V;
      await Task.Delay(0);
      t := t + f();
    end;
    return t;
  end Sum;
  public static method Gen(ll: LinkedList<int>, s: string, items: Array<Item>): IEnumerable<int>
    for var b in ll do
      var f := lambda(): int => b * 2;
      yield return f();
      yield return 0;
    end;
    for var ch in s do
      var f := lambda(): int => ch.ToString().Length;
      yield return 0;
      yield return f();
    end;
    for var e in Src(10) do
      var f := lambda(): int => e * 3;
      yield return 0;
      yield return f();
    end;
    for var it in items do
      var f := lambda(): int => it.V;
      yield return 0;
      yield return f();
    end;
  end Gen;
  public static async method Main(): Task<int>
    var ll := new LinkedList<int>();
    for var i in 0..<10 do
      ll.AddLast(i);
    end;
    using var i1 := new Item(7);
    using var i2 := new Item(11);
    var items: Array<Item> := [i1, i2];
    var a := 0;
    var b := 0;
    for var k in 0..<200 do
      a := a + await Sum(ll, "hello", items);
      for var v in Gen(ll, "hello", items) do b := b + v; end;
    end;
    Console.WriteLine(a);
    Console.WriteLine(b);
    return 0;
  end Main;
end C;)BZL"),
        "49600\n49600\n");
}
