// Regression tests for issue #5274 - §28.3 K3.1 / K7.4 and §28.8 (v0.313).
//
// A store of a bare `owned` PARAMETER into an `owned` field is a consume (§28.8:
// "moves into owned field"; consume-table row K3.1: the source is moved-from, so
// "the scope-end disposal skips it"). For an `owned` field of a VTABLE-LESS
// builtin class (Stopwatch, Mutex, ...) the parameter's slot was not nulled at
// the store (the #5218 null-store gate recognised only class / collection / Task
// fields), so the §28.3 method-end auto-drop released the object and the
// container's disposer released the field again: ASan double-free. Every row
// loops 200x; `expectClean5274` checks the exact stdout through runAot and
// runAotChecked (the checked-disposal / ASan / LSan leg).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectClean5274(const char* src, const std::string& expected) {
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

// The issue repro verbatim (modulo the stdout report): ctor stores an owned
// Stopwatch param into an owned field.
BEZEL_TEST(owned_param_store_vtableless_5274, issue_repro_stopwatch_ctor_store) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
class H
  public owned v: Stopwatch;
  public constructor(owned x: Stopwatch) v := x; end constructor;
end H;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var b := new H(Stopwatch.StartNew());
      if b.v.IsRunning then n := n + 1; end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "200\n");
}

// The `move w` spelling of the same store.
BEZEL_TEST(owned_param_store_vtableless_5274, move_local_into_ctor) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
class H
  public owned v: Stopwatch;
  public constructor(owned x: Stopwatch) v := x; end constructor;
end H;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      var w := new Stopwatch();
      using var b := new H(move w);
      b.v.Start();
      if b.v.IsRunning then n := n + 1; end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "200\n");
}

// The heap-use-after-free spelling: a fresh `new Stopwatch()` argument, then a
// read of the field (the pre-fix auto-drop had already freed it).
BEZEL_TEST(owned_param_store_vtableless_5274, fresh_argument_then_field_use) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
class H
  public owned v: Stopwatch;
  public constructor(owned x: Stopwatch) v := x; end constructor;
end H;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var b := new H(new Stopwatch());
      b.v.Start();
      if b.v.IsRunning then n := n + 1; end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "200\n");
}

// The store in an ordinary METHOD, not a constructor.
BEZEL_TEST(owned_param_store_vtableless_5274, store_in_method) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
class H
  public owned v: Stopwatch?;
  public method Put(owned x: Stopwatch) v := x; end Put;
end H;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var b := new H();
      b.Put(new Stopwatch());
      if b.v <> null then n := n + 1; end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "200\n");
}

// Nullable owned field: `owned v: Stopwatch?`, stored from an owned param in a
// ctor.
BEZEL_TEST(owned_param_store_vtableless_5274, nullable_owned_field) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
class H
  public owned v: Stopwatch?;
  public constructor(owned x: Stopwatch) v := x; end constructor;
end H;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var b := new H(Stopwatch.StartNew());
      if b.v <> null then n := n + 1; end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "200\n");
}

// Reassigning an owned field that already holds a value, USER-CLASS control:
// the displaced Item is disposed once (§28.7). The Stopwatch spelling of this
// row cannot be pinned here: a vtable-less builtin owned field is not
// drop-on-overwrite at all, a separate leak filed as #5325.
BEZEL_TEST(owned_param_store_vtableless_5274, reassign_field_holding_value_class_control) {
    const char* src = R"BZL(
module M;
class Item
  public static n: int := 0;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
class H
  public owned v: Item?;
  public method Put(owned x: Item) v := x; end Put;
end H;
class P
  public static method Main(): int
    for var i in 0..199 do
      using var b := new H();
      b.Put(new Item());
      b.Put(new Item());
    end for;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "400\n");
}

// §28.3 K7.4: a CONDITIONAL store joins to maybe-consumed and "scope-end
// disposal is per-path" - the stored path leaves the parameter to the field, the
// path that does not store auto-drops it. Each Stopwatch is freed exactly once
// on both paths.
BEZEL_TEST(owned_param_store_vtableless_5274, conditional_store_per_path) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
class H
  public owned v: Stopwatch?;
  public method Put(owned x: Stopwatch, c: bool)
    if c then v := x; end;
  end Put;
end H;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var b := new H();
      b.Put(new Stopwatch(), i < 100);
      if b.v <> null then n := n + 1; end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "100\n");
}

// The same defect across the other vtable-less builtin kinds that are
// constructible: each holder stores its owned param into an owned field.
BEZEL_TEST(owned_param_store_vtableless_5274, other_vtableless_builtin_kinds) {
    const char* src = R"BZL(
module M;
import System.Threading;
class HMutex
  public owned v: Mutex;
  public constructor(owned x: Mutex) v := x; end constructor;
end HMutex;
class HSem
  public owned v: Semaphore;
  public constructor(owned x: Semaphore) v := x; end constructor;
end HSem;
class HCts
  public owned v: CancellationTokenSource;
  public constructor(owned x: CancellationTokenSource) v := x; end constructor;
end HCts;
class HSb
  public owned v: StringBuilder;
  public constructor(owned x: StringBuilder) v := x; end constructor;
end HSb;
class HRnd
  public owned v: Random;
  public constructor(owned x: Random) v := x; end constructor;
end HRnd;
class HAtom
  public owned v: AtomicInteger<int>;
  public constructor(owned x: AtomicInteger<int>) v := x; end constructor;
end HAtom;
class HChan
  public owned v: Channel<int>;
  public constructor(owned x: Channel<int>) v := x; end constructor;
end HChan;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var a := new HMutex(new Mutex());
      using var b := new HSem(new Semaphore(1));
      using var c := new HCts(new CancellationTokenSource());
      using var d := new HSb(new StringBuilder());
      using var e := new HRnd(new Random());
      using var f := new HAtom(new AtomicInteger<int>(5));
      using var g := new HChan(new Channel<int>());
      d.v.Append("x");
      if d.v.Length = 1 and f.v.Value = 5 and not c.v.IsCancellationRequested
          and not g.v.IsClosed then
        n := n + 1;
      end;
    end for;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    expectClean5274(src, "200\n");
}

// Controls the fix must not disturb: a user class, an interface-typed field and
// a generic `T` (at a user class and at Stopwatch), each stored from an owned
// param; the destructor count pins exactly-once disposal.
BEZEL_TEST(owned_param_store_vtableless_5274, class_interface_generic_controls) {
    const char* src = R"BZL(
module M;
import System.Diagnostics;
interface IThing
  method Tag(): int;
end IThing;
class Item: IThing
  public static n: int := 0;
  public method Tag(): int => 1;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
class HClass
  public owned v: Item;
  public constructor(owned x: Item) v := x; end constructor;
end HClass;
class HIface
  public owned v: IThing;
  public constructor(owned x: IThing) v := x; end constructor;
end HIface;
class Box<T>
  public owned v: T;
  public constructor(owned x: T) v := x; end constructor;
end Box;
class P
  public static method Main(): int
    var n := 0;
    for var i in 0..199 do
      using var a := new HClass(new Item());
      using var b := new HIface(new Item());
      using var c := new Box<Item>(new Item());
      using var d := new Box<Stopwatch>(Stopwatch.StartNew());
      if a.v.Tag() = 1 and b.v.Tag() = 1 and c.v.Tag() = 1 and d.v.IsRunning then
        n := n + 1;
      end;
    end for;
    Console.WriteLine(n.ToString());
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end P;
)BZL";
    // 3 Items per iteration, each destroyed exactly once: 600.
    expectClean5274(src, "200\n600\n");
}
