// Regression tests for issue #5218 - §28.3 *Owned-local discharge requirement*
// (affine auto-drop, "scope-end disposal is per-path", K1.4 / K7.4, v0.313).
//
// An `owned` parameter stored into an `owned` field on only ONE branch was
// still auto-dropped unconditionally at method end, freeing the object the
// field now owns (heap-use-after-free). The parameter's slot is now nulled at
// the transfer, so the null-guarded auto-drop is per-path: the moved path
// skips it, the untouched path still disposes. Each row loops 200x and counts
// destructor runs exactly; runAotChecked adds the checked-disposal / ASan leg.
// A cross-module shape is verified by probe (lib + app), the codegen harness
// being single-module.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectClean5218(const char* src, const std::string& expected) {
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

// The issue repro: `if c then _x := v; end;` stores the owned parameter into an
// owned field on ONE branch. 100 calls store (the field disposes the displaced
// value), 100 do not (the §28.3 method-end auto-drop disposes), then 200 Put2
// calls store unconditionally: every one of the 400 Items is destroyed once.
BEZEL_TEST(owned_param_branch_store_5218, issue_repro_branch_store_loop_200) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  public acc: int;
  private owned _x: Item?;
  public method Put(owned v: Item, c: bool)
    if c then _x := v; end;
  end Put;
  public method Put2(owned v: Item)
    _x := v;
  end Put2;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i < 100); end;
      for var i in 0..<200 do b.Put2(new Item(i)); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "400\n");
}

// Store on the then-branch, or on the else-branch: the parameter is moved on
// every path that stores and auto-dropped on the one that does not.
BEZEL_TEST(owned_param_branch_store_5218, else_branch_store_both_paths) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  private owned _x: Item?;
  private owned _y: Item?;
  public method Put(owned v: Item, c: bool)
    if c then _x := v; else _y := v; end;
  end Put;
  public method PutNone(owned v: Item, c: bool)
    if c then _x := v; else if not c then _y := v; end; end;
  end PutNone;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i < 100); end;
      for var i in 0..<200 do b.PutNone(new Item(i), i < 100); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "400\n");
}

// Nested `if`: only the innermost taken path moves the parameter.
BEZEL_TEST(owned_param_branch_store_5218, nested_if_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  private owned _x: Item?;
  public method Put(owned v: Item, a: bool, c: bool)
    if a then
      if c then _x := v; end;
    end;
  end Put;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i < 150, i mod 2 = 0); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}

// A store followed by an early `return` on one path; the fall-through path
// auto-drops.
BEZEL_TEST(owned_param_branch_store_5218, early_return_after_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  private owned _x: Item?;
  public method Put(owned v: Item, c: bool)
    if c then
      _x := v;
      return;
    end;
    Item.n := Item.n + 0;
  end Put;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i < 100); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}

// A `case` arm stores the parameter; the other arms leave it to the auto-drop.
BEZEL_TEST(owned_param_branch_store_5218, case_arm_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  private owned _x: Item?;
  public method Put(owned v: Item, k: int)
    case k of
      0 => _x := v,
      1 => Item.n := Item.n + 0,
      _ => Item.n := Item.n + 0
    end;
  end Put;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i mod 3); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}

// A conditional store inside `try` with a `finally` clause.
BEZEL_TEST(owned_param_branch_store_5218, try_finally_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  private owned _x: Item?;
  public method Put(owned v: Item, c: bool)
    try
      if c then _x := v; end;
    finally
      Item.n := Item.n + 0;
    end;
  end Put;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i < 100); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}

// Virtual dispatch: the override stores conditionally.
BEZEL_TEST(owned_param_branch_store_5218, virtual_override_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public abstract class A
  public abstract method Put(owned v: Item, c: bool);
end A;
public class B: A
  private owned _x: Item?;
  public override method Put(owned v: Item, c: bool)
    if c then _x := v; end;
  end Put;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      var a: A := b;
      for var i in 0..<200 do a.Put(new Item(i), i < 100); end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}

// A constructor owned parameter stored conditionally.
BEZEL_TEST(owned_param_branch_store_5218, ctor_param_conditional_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  private owned _x: Item?;
  public constructor(owned v: Item, c: bool)
    if c then _x := v; end;
  end constructor;
end B;
public class Program
  public static method Main(): int
    for var i in 0..<200 do
      using var b := new B(new Item(i), i < 100);
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}

// An owned collection parameter (OwningArray) stored conditionally into an
// owned collection field; each element is destroyed exactly once.
BEZEL_TEST(owned_param_branch_store_5218, owned_collection_param_branch_store) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class B
  private owned _a: OwningArray<Item>?;
  public method Put(owned a: OwningArray<Item>, c: bool)
    if c then _a := a; end;
  end Put;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do
        var a := new OwningArray<Item>();
        a.Add(new Item(i));
        b.Put(move a, i < 100);
      end;
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5218(src, "200\n");
}
