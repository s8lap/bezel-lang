// Regression tests for issue #5209 - §23.3 *Explicit setter parameter
// modifier* and §28.3 *Unbound owned temporaries* (v0.313).
//
// #5119 fixed the inlined-setter path; the virtual/abstract-dispatched setter
// (lowerVirtualPropertySet) and the imported cross-module setter
// (lowerImportedPropertySetValue) still leaked the fresh temporary in
// `obj.P := new C(...)` through a plain setter (value is a borrow, §23.3;
// disposed at the end of the full expression, §28.3). The imported path needs
// two modules; this codegen harness is single-module, so it is verified by
// probe (lib.bzl + app.bzl) rather than a row here.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectClean5209(const char* src, const std::string& expected) {
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

// Virtual dispatch, plain setter, 200 fresh temporaries: each destroyed once.
BEZEL_TEST(setter_fresh_temp_dispose_5209, virtual_plain_setter_loop_200) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public abstract class A
  public abstract property Val: Item
    get;
    set;
  end Val;
end A;
public class B: A
  private _v: Item?;
  public acc: int;
  public override property Val: Item
    get => _v!;
    set => do acc := acc + value.tag; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      var v: A := b;
      for var i in 0..<200 do v.Val := new Item(i); end;
      Console.WriteLine(b.acc.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5209(src, "19900\n200\n");
}

// Control: borrowed RHS is not disposed by the assignment (only `it` at scope end).
BEZEL_TEST(setter_fresh_temp_dispose_5209, virtual_borrowed_rhs_not_disposed) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public abstract class A
  public abstract property Val: Item
    get;
    set;
  end Val;
end A;
public class B: A
  private _v: Item?;
  public acc: int;
  public override property Val: Item
    get => _v!;
    set => do acc := acc + value.tag; end;
  end Val;
end B;
public class Program
  public static method Put(v: A, it: Item)
    v.Val := it;
  end Put;
  public static method Main(): int
    do
      using var it := new Item(3);
      using var b := new B();
      var v: A := b;
      for var i in 0..<200 do Put(v, it); end;
      Console.WriteLine(b.acc.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5209(src, "600\n1\n");
}

// set(owned) takes the value over: no caller-side dispose (no double dispose).
// The setter stores `value` in an owned field (disposing the displaced one),
// so each Item is destroyed exactly once: 200 destructions.
BEZEL_TEST(setter_fresh_temp_dispose_5209, virtual_set_owned_no_double_dispose) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public abstract class A
  public abstract property Val: Item
    get;
    set(owned);
  end Val;
end A;
public class B: A
  private owned _v: Item?;
  public acc: int;
  public override property Val: Item
    get => _v!;
    set(owned) => do acc := acc + value.tag; _v := value; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      var v: A := b;
      for var i in 0..<200 do v.Val := new Item(i); end;
      Console.WriteLine(b.acc.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5209(src, "19900\n200\n");
}

// Interface-dispatched plain setter (the isIfaceProp arm of
// lowerVirtualPropertySet), class implementer: each temporary destroyed once.
BEZEL_TEST(setter_fresh_temp_dispose_5209, interface_plain_setter_loop_200) {
    const char* src = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public interface IA
  property Val: Item
    get;
    set;
  end Val;
end IA;
public class B: IA
  private _v: Item?;
  public acc: int;
  public property Val: Item
    get => _v!;
    set => do acc := acc + value.tag; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      var v: IA := b;
      for var i in 0..<200 do v.Val := new Item(i); end;
      Console.WriteLine(b.acc.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";
    expectClean5209(src, "19900\n200\n");
}
