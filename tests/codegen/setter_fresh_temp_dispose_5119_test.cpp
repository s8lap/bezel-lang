// Regression tests for issue #5119 — §28.3 *Unbound owned temporaries* and
// §23.3 *Explicit setter parameter modifier* (v0.313).
//
// §23.3: "The setter's implicit `value` parameter has no modifier by default
// and takes a borrow of the property's declared type." §28.3: an owned value
// "used only by borrowing operations within the expression — ... as a
// non-`owned` argument ... is disposed at the end of the full expression".
// So `b.Val := new Item(i)` through a plain setter disposes the fresh Item at
// the end of the assignment statement, exactly as `b.Put(new Item(i))` does.
//
// THE DEFECT (measured on origin/main d1827572, --probe --asan): 50 such
// assignments leaked 50 Items (800 B), for a bare-`T` property at `T = Item`
// AND for the concrete non-generic `Item` twin alike — the inlined setter
// path (lowerPropertySetValue) never disposed the temporary. The issue's
// "bare-T" framing is incidental: `_v: T` is NOT an `owned` field (§28.5),
// so it never disposes a displaced value (§28.7 is scoped to `owned`
// fields); a store-time dispose there would double-free the caller's
// temporary (the UAF the first lane measured).
//
// No row reads the property after its last temporary died: the stored
// borrow dangles from the statement end on (§28.3's borrowed-field store
// rule, K4.3 trust residual), which is why the issue's literal repro —
// which reads `b.Val.tag` afterwards — is not used verbatim.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectClean5119(const char* src, const std::string& expected) {
    auto plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
    auto checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// The issue's shape (bare-T block setter at T = Item), 200 iterations; the
// destructor count proves each temporary is disposed exactly once.
BEZEL_TEST(setter_fresh_temp_dispose_5119, bare_t_block_setter_loop_200) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public property Val: T
    get => _v;
    set => do _v := value; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    var sum := 0;
    do
      using var b := new B<Item>(new Item(0));
      for var i in 0..<200 do
        b.Val := new Item(i);
        sum := sum + i;
      end;
    end;
    Console.WriteLine(Item.n.ToString());
    Console.WriteLine(sum.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5119(src, "201\n19900\n");
}

// The concrete twin: a non-generic `Item` property leaked identically.
BEZEL_TEST(setter_fresh_temp_dispose_5119, concrete_block_setter_loop_200) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class B
  private _v: Item?;
  public acc: int;
  public property Val: Item
    get => _v!;
    set => do acc := acc + value.tag; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    var acc := 0;
    do
      using var b := new B();
      for var i in 0..<200 do
        b.Val := new Item(i);
      end;
      acc := b.acc;
    end;
    Console.WriteLine(Item.n.ToString());
    Console.WriteLine(acc.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5119(src, "200\n19900\n");
}

// Control: a parameter-sourced (borrowed) RHS is not a fresh producer and
// must NOT be disposed by the assignment — the caller still owns it.
BEZEL_TEST(setter_fresh_temp_dispose_5119, borrowed_rhs_not_disposed) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public property Val: T
    get => _v;
    set => do _v := value; end;
  end Val;
end B;
public class Program
  public static method Fill(b: B<Item>, it: Item)
    for var i in 0..<200 do
      b.Val := it;
    end;
  end Fill;
  public static method Main(): int
    var t := 0;
    do
      using var it := new Item(7);
      using var b := new B<Item>(it);
      Fill(b, it);
      t := b.Val.tag;
    end;
    Console.WriteLine(Item.n.ToString());
    Console.WriteLine(t.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5119(src, "1\n7\n");
}
