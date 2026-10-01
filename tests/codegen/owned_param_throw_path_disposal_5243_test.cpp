// Regression tests for issue #5243 - §28.3 *Owned-local discharge requirement*
// (affine auto-drop, "disposed exactly once on every path", K1.4 / K7.4, v0.313).
//
// An `owned` parameter stored into an `owned` field on ONE branch while the
// other branch `throw`s was never disposed on the throwing path: the codegen
// UnwindIneligibleWalker tainted the parameter as consumed by the field store,
// so no unwind disposal was emitted (LeakSanitizer: the parameter leaks).
// #5218 nulls the parameter slot at the field store, which makes the
// null-guarded unwind disposal exactly-once, so the walker now leaves such a
// parameter unwind-eligible: a throw before / beside the store disposes it, a
// throw after the store finds the null slot and skips it. Each row loops 200x
// and counts destructor runs exactly on stdout; runAotChecked adds the
// checked-disposal / ASan leg.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectClean5243(const char* src, const std::string& expected) {
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

// The issue repro: store on one branch, throw on the other. 100 calls store
// (the field disposes the displaced value, then `using` disposes the last),
// 100 throw (the unwind disposal disposes the parameter): 200 destructor runs.
BEZEL_TEST(owned_param_throw_path_5243, issue_repro_store_or_throw_loop_200) {
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
    if c then _x := v; else throw new Exception("no"); end;
  end Put;
end B;
public class Program
  public static method Main(): int
    var caught := 0;
    do
      using var b := new B();
      for var i in 0..<200 do
        try
          b.Put(new Item(i), i < 100);
        catch
          Exception => caught := caught + 1;
        end try;
      end;
    end;
    Console.WriteLine(caught.ToString());
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5243(src, "100\n200\n");
}

// Store-only path, no throw anywhere in the body of the call: the store path
// still disposes exactly once (no double dispose with #5218's slot nulling).
BEZEL_TEST(owned_param_throw_path_5243, store_path_no_throw_disposes_once) {
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
    if c then _x := v; else throw new Exception("no"); end;
  end Put;
end B;
public class Program
  public static method Main(): int
    var caught := 0;
    do
      using var b := new B();
      for var i in 0..<200 do
        try
          b.Put(new Item(i), true);
        catch
          Exception => caught := caught + 1;
        end try;
      end;
    end;
    Console.WriteLine(caught.ToString());
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5243(src, "0\n200\n");
}

// A throw AFTER the store: the field owns the value, the nulled parameter slot
// makes the unwind disposal a no-op, so each Item is destroyed exactly once.
BEZEL_TEST(owned_param_throw_path_5243, throw_after_store_disposes_once) {
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
    if c then _x := v; end;
    if c then throw new Exception("after"); end;
  end Put;
end B;
public class Program
  public static method Main(): int
    var caught := 0;
    do
      using var b := new B();
      for var i in 0..<200 do
        try
          b.Put(new Item(i), i < 100);
        catch
          Exception => caught := caught + 1;
        end try;
      end;
    end;
    Console.WriteLine(caught.ToString());
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5243(src, "100\n200\n");
}

// Two owned parameters, a throw before any store: both are disposed on the
// unwind path; on the store path one goes to the field and the other is
// auto-dropped at method end. 200 calls x 2 Items = 400 destructor runs.
BEZEL_TEST(owned_param_throw_path_5243, two_owned_params_throw_before_store) {
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
  public method Put(owned a: Item, owned b: Item, keep: bool)
    if not keep then throw new Exception("early"); end;
    _x := a;
  end Put;
end B;
public class Program
  public static method Main(): int
    var caught := 0;
    do
      using var b := new B();
      for var i in 0..<200 do
        try
          b.Put(new Item(i), new Item(i), i < 100);
        catch
          Exception => caught := caught + 1;
        end try;
      end;
    end;
    Console.WriteLine(caught.ToString());
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5243(src, "100\n400\n");
}

// A try-local variant: the method catches its own throw, so the call returns
// normally on the no-store path and the parameter is disposed exactly once.
BEZEL_TEST(owned_param_throw_path_5243, method_catches_own_throw) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;

public class B
  public caught: int;
  private owned _x: Item?;
  public method Put(owned v: Item, c: bool)
    try
      if c then _x := v; else throw new Exception("no"); end;
    catch
      Exception => caught := caught + 1;
    end try;
  end Put;
end B;
public class Program
  public static method Main(): int
    var seen := 0;
    do
      using var b := new B();
      for var i in 0..<200 do b.Put(new Item(i), i < 100); end;
      seen := b.caught;
    end;
    Console.WriteLine(seen.ToString());
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5243(src, "100\n200\n");
}
