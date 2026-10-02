// Regression tests for issue #5252 - §23.3 *Explicit setter parameter
// modifier* ("`set(owned)` ... takes owned T") and §28.3 (spec v0.313)
// borrowed-field store rule, clause 2: "an `owned` parameter is scope-bound
// ... so a borrow of it is rejected here exactly as a borrow of a scope-bound
// local is".
//
// Accessor bodies never ran the full ownership pass, so in a `set(owned)`
// accessor `_x := value` and `_x := value.Self()` (a borrow of the owned
// `value` stored into a NON-owned field) were admitted, while the same
// statements in a method with an `owned` parameter are rejected (#5225). On
// staging every stored value leaked; once #5212's setter auto-drop lands it is
// a use-after-free. The fix runs the ownership pass over `set(owned)` bodies
// with `value` as an `owned` parameter. The accept rows pin that a correct
// owned setter, a plain setter and a read-write indexer are unaffected.

#include <algorithm>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The issue's literal repro prelude (/var/tmp/bezel-scratch/r25/p5225/p/).
const char* kPrelude5252 = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method Self(): Item return this; end Self;
  public method Reg(r: Reg) r.last := this; end Reg;
  public method Use(): int return tag; end Use;
end Item;
public class Reg
  public last: Item?;
end Reg;
public class Helper
  public static method Borrow(x: Item): int return x.tag; end Borrow;
  public static method Take(owned x: Item): int return x.tag; end Take;
end Helper;
)BZL";

const char* kClassHead5252 = R"BZL(
public class B
  public acc: int;
  private _x: Item?;
  private owned _reg: Reg := new Reg();
  private owned _o: Item?;
  private owned _list: OwningArray<Item> := new OwningArray<Item>();
)BZL";

// The issue repro's driver: 200 stores through `b.Val`, then the destructor
// count after `b` is disposed.
const char* kMain5252 = R"BZL(
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Val := new Item(i); end;
      Console.WriteLine(b.acc.ToString()); Console.WriteLine(b.Val.tag.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL";

std::string program5252(const char* property) {
    return std::string(kPrelude5252) + kClassHead5252 + property +
           "end B;\n" + kMain5252;
}

void expectSetterFieldStoreRejectResult5252(const bezel::test::JitResult& r,
                                            const char* name = "value") {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle = std::string("borrow of `") + name +
                               "` stored into a field would outlive its source";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
    const std::string clause = std::string("`") + name +
                               "` is an `owned` parameter, whose value THIS "
                               "method disposes at its end";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, clause.c_str()));
}

void expectSetterFieldStoreReject5252(const std::string& src,
                                      const char* name = "value") {
    expectSetterFieldStoreRejectResult5252(runAot(src), name);
}

}  // namespace

// Issue repro b_setter.bzl: a receiver-sourced return over `value` stored
// into the non-owned field `_x`.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_28_3_issue_repro_value_self_rejected) {
    expectSetterFieldStoreReject5252(program5252(R"BZL(
  public property Val: Item
    get => _x!;
    set(owned) => do
      _x := value.Self(); acc := acc + 1;
    end;
  end Val;
)BZL"));
}

// Issue repro f_setter_direct.bzl: the direct store `_x := value`.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_28_3_issue_repro_value_direct_rejected) {
    expectSetterFieldStoreReject5252(program5252(R"BZL(
  public property Val: Item
    get => _x!;
    set(owned) => do
      _x := value; acc := acc + 1;
    end;
  end Val;
)BZL"));
}

// The single-statement accessor spelling (`=> stmt;`) of the same store. The
// parser synthesizes that body's assignment statement; the diagnostic must
// carry the statement's real line:column (it reported 0:0 before the parser
// stamped the synthesized statement's span).
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_28_3_single_statement_setter_rejected) {
    const std::string src = program5252(R"BZL(
  public property Val: Item
    get => _x!;
    set(owned) => _x := value;
  end Val;
)BZL");
    auto r = runAot(src);
    expectSetterFieldStoreRejectResult5252(r);
    const std::string stmt = "set(owned) => _x := value;";
    const std::size_t at = src.find(stmt);
    BEZEL_EXPECT_TRUE(at != std::string::npos);
    if (at == std::string::npos) return;
    const std::size_t lineStart = src.rfind('\n', at) + 1;
    const int line = 1 + static_cast<int>(
        std::count(src.begin(), src.begin() + static_cast<long>(at), '\n'));
    const int col = 1 + static_cast<int>(at - lineStart) +
                    static_cast<int>(std::string("set(owned) => ").size());
    const std::string located = ":" + std::to_string(line) + ":" +
                                std::to_string(col) +
                                ": borrow of `value` stored into a field";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, located.c_str()));
}

// §23.3: `set(owned x)` renames the parameter; the rule follows the name.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_renamed_owned_setter_param_rejected) {
    expectSetterFieldStoreReject5252(program5252(R"BZL(
  public property Val: Item
    get => _x!;
    set(owned x) => do
      _x := x.Self(); acc := acc + 1;
    end;
  end Val;
)BZL"),
                                     "x");
}

// Control: the #5225 method form is still rejected with the same text.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_28_3_method_form_control_rejected) {
    expectSetterFieldStoreReject5252(std::string(kPrelude5252) + R"BZL(
public class C
  private _x: Item?;
  public method SetV(owned value: Item) _x := value.Self(); end SetV;
end C;
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Control (accept): a correct `set(owned)` transfers `value` into an `owned`
// field (a marker-free K3.1 position, so no `move`). 200 stores: 199 displaced
// values are disposed on overwrite (§28.7) and the last when `b` is disposed,
// so every Item is destroyed exactly once.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_owned_field_transfer_accepted_no_leak) {
    auto r = runAot(program5252(R"BZL(
  public property Val: Item
    get => _o!;
    set(owned) => do
      _o := value; acc := acc + 1;
    end;
  end Val;
)BZL"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n199\n200\n"));
}

// Control (accept): a `set(owned)` that only BORROWS `value` (a plain-argument
// read, a non-escaping receiver read) keeps the existing acceptance.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_owned_setter_borrow_reads_accepted) {
    auto r = runAot(program5252(R"BZL(
  public property Val: Item
    get => _o!;
    set(owned) => do
      acc := acc + Helper.Borrow(value) + value.Use() - 2 * value.tag + 1;
      _o := value;
    end;
  end Val;
)BZL"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n199\n200\n"));
}

// Control (accept): plain (borrow) setters on int and string fields.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_plain_setter_value_types_accepted) {
    auto r = runAot(R"BZL(
module M;
public class S
  private _n: int;
  private _s: string := "";
  public property N: int
    get => _n;
    set => _n := value;
  end N;
  public property Name: string
    get => _s;
    set => _s := value;
  end Name;
end S;
public class Program
  public static method Main(): int
    var s := new S();
    s.N := 41; s.N := s.N + 1;
    s.Name := "ab"; s.Name := s.Name + "c";
    Console.WriteLine(s.N.ToString()); Console.WriteLine(s.Name);
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\nabc\n"));
}

// Control (accept): a read-write indexer with a plain setter, and an owning
// `set(owned)` indexer storing `value` into an owning-collection slot (K3.1).
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_read_write_indexers_accepted) {
    auto r = runAot(R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class Grid
  private _cells: Array<int> := [0, 0, 0];
  public property [i: int]: int
    get => _cells[i];
    set => _cells[i] := value;
  end;
end Grid;
public class Shelf
  private owned _items: OwningArray<Item> := new OwningArray<Item>();
  public constructor()
    _items.Add(new Item(0)); _items.Add(new Item(0));
  end constructor;
  public property [i: int]: Item
    get => _items[i];
    set(owned) => _items[i] := value;
  end;
end Shelf;
public class Program
  public static method Main(): int
    var g := new Grid();
    g[1] := 5; g[1] += 2; g[2] := g[1] * 3;
    Console.WriteLine((g[0] + g[1] + g[2]).ToString());
    do
      using var sh := new Shelf();
      for var i in 0..<100 do sh[i mod 2] := new Item(i); end;
      Console.WriteLine((sh[0].tag + sh[1].tag).ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("28\n197\n102\n"));
}

// Control (accept): a CONDITIONAL transfer of `value` into an `owned` field.
// The method form (`method SetV(owned value: Item)` with the same body) is
// admitted with the §28.3 must-use warning; the setter must not be stricter.
// Driven only on the consuming path so the program is leak-free today
// (setter auto-drop of the other path is #5212).
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_28_3_conditional_owned_field_transfer_accepted) {
    auto r = runAot(std::string(kPrelude5252) + kClassHead5252 + R"BZL(
  public property Val: Item
    get => _o!;
    set(owned) => do
      if acc mod 2 = 0 then _o := value; end; acc := acc + 1;
    end;
  end Val;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      b.Val := new Item(7);
      Console.WriteLine(b.acc.ToString()); Console.WriteLine(b.Val.tag.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n7\n1\n"));
}

// §23.3 (v0.313): "The setter parameter ... may be shadowed inside the setter
// body". A block-local `var value` that ends before the store must not change
// what the later `value` names: the store transfers the accessor's own owned
// `value` (K3.1) and is accepted. (The ownership pass's by-name state lookup
// answered with the shadowing int local's state and rejected it.) 200 stores:
// acc = 200 * 5, the last stored tag is 199, every Item is destroyed once.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_shadowed_value_owned_transfer_accepted) {
    auto r = runAot(program5252(R"BZL(
  public property Val: Item
    get => _o!;
    set(owned) => do
      do var value := 5; acc := acc + value; end; _o := value;
    end;
  end Val;
)BZL"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n199\n200\n"));
}

// The same shadow, then a BORROW of the accessor's owned `value` stored into
// the non-owned field `_x`: still the #5252 rejection (the shadow does not
// hide the escape).
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_3_28_3_shadowed_value_borrow_escape_rejected) {
    expectSetterFieldStoreReject5252(program5252(R"BZL(
  public property Val: Item
    get => _x!;
    set(owned) => do
      do var value := 5; acc := acc + value; end; _x := value;
    end;
  end Val;
)BZL"));
}

// §22.3 (v0.286) / §17.8 (v0.309): passing the named owned binding `value` to
// an `owned` formal consumes it, and the call site must announce that with
// `move`. Before #5252 setter bodies were never checked, so the unmarked call
// was admitted.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_22_3_owned_setter_value_unmarked_consume_rejected) {
    auto r = runAot(program5252(R"BZL(
  public property Val: Item
    get => _o!;
    set(owned) => do
      acc := acc + Helper.Take(value);
    end;
  end Val;
)BZL"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "this call consumes `value`; write `M(move value)`"));
}

// The `move`-marked spelling of the same consuming call is accepted: every
// Item is consumed (and disposed) by `Take`; acc = 0 + 1 + ... + 199.
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_22_3_owned_setter_value_move_consume_accepted) {
    auto r = runAot(std::string(kPrelude5252) + kClassHead5252 + R"BZL(
  public property Val: Item
    get => _o!;
    set(owned) => do
      acc := acc + Helper.Take(move value);
    end;
  end Val;
end B;
public class Program
  public static method Main(): int
    do
      using var b := new B();
      for var i in 0..<200 do b.Val := new Item(i); end;
      Console.WriteLine(b.acc.ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("19900\n200\n"));
}

// §23.4: an indexer's `set(owned)` with an index parameter is checked the same
// way: storing a borrow of `value` into the non-owned field `_last` is
// rejected (the slot store of a fresh `new` beside it is fine).
BEZEL_TEST(accessor_set_owned_field_store_5252,
           spec_23_4_28_3_owned_indexer_setter_field_store_rejected) {
    expectSetterFieldStoreReject5252(R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class Shelf
  private owned _items: OwningArray<Item> := new OwningArray<Item>();
  private _last: Item?;
  public constructor()
    _items.Add(new Item(0)); _items.Add(new Item(0));
  end constructor;
  public property [i: int]: Item
    get => _items[i];
    set(owned) => do
      _last := value;
      _items[i] := new Item(i);
    end;
  end;
end Shelf;
public class Program
  public static method Main(): int
    do
      using var sh := new Shelf();
      for var i in 0..<100 do sh[i mod 2] := new Item(i); end;
      Console.WriteLine((sh[0].tag + sh[1].tag).ToString());
    end;
    Console.WriteLine(Item.n.ToString());
    return 0;
  end Main;
end Program;
)BZL");
}
