// Regression tests for issue #5236 - §28.3 K3.1 consume position ("store into
// an `owned` field or owning-collection slot, including through a
// `set(owned)` accessor (§23.3)"), §28.8 (a borrow cannot be passed where an
// `owned` value is required) and A4 (accessor read faces are lend-only), spec
// v0.313.
//
// Sema classified `b.Val := x` through a `set(owned)` accessor as a plain
// borrowed store and never applied the §28.8 owned-sink check, so a borrow
// RHS (a non-owned parameter, a getter read) was admitted. The ordinary
// `owned`-parameter method and the owning-collection `set(owned)` indexer
// already rejected it. Blocks #5212 (setter-end auto-drop would turn the
// admitted borrow into a heap-use-after-free).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kPre5236 = R"BZL(
module M;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class Maker
  public static method Make(t: int): owned Item return new Item(t); end Make;
end Maker;
public class B
  public acc: int;
  public owned keep: Item?;
)BZL";

const char* kProp5236 = R"BZL(
  public property Val: Item
    get => keep!;
    set(owned) => do acc := acc + value.tag; end;
  end Val;
end B;
)BZL";

void expectOwnedSinkReject5236(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow cannot be passed where an `owned` value is required"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xc2\xa7""28.8"));
}

}  // namespace

// Repro 1 (c07): a non-owned parameter stored through a `set(owned)` property.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, borrow_param_rejected) {
    expectOwnedSinkReject5236(std::string(kPre5236) + kProp5236 + R"BZL(
public class Program
  public static method Feed(b: B, x: Item) b.Val := x; end Feed;
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Repro 2 (c08): the RHS is a getter read (A4: an accessor read is a borrow),
// stored back through the implicit-`this` `set(owned)` property.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, getter_self_assign_rejected) {
    expectOwnedSinkReject5236(std::string(kPre5236) +
                              "  public method Re() Val := Val; end Re;\n" +
                              kProp5236 + R"BZL(
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Same rule through `this.Val` with a borrow-parameter RHS.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, this_qualified_borrow_rejected) {
    expectOwnedSinkReject5236(std::string(kPre5236) +
                              "  public method Put(x: Item) this.Val := x; end Put;\n" +
                              kProp5236 + R"BZL(
public class Program
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Indexer `set(owned)` path (user indexer): a borrow RHS is rejected.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, indexer_borrow_rejected) {
    expectOwnedSinkReject5236(R"BZL(
module M;
public class Item
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
end Item;
public class Bag
  public owned keep: Item?;
  public property [i: int]: Item
    get => keep!;
    set(owned) => do keep := value; end;
  end;
end Bag;
public class Program
  public static method Feed(g: Bag, x: Item) g[0] := x; end Feed;
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Interface (virtual) accessor path takes the same rule.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, interface_property_borrow_rejected) {
    expectOwnedSinkReject5236(R"BZL(
module M;
public class Item
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
end Item;
public interface IHold
  property Val: Item
    get;
    set(owned);
  end Val;
end IHold;
public class Program
  public static method Feed(h: IHold, x: Item) h.Val := x; end Feed;
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Accept controls: fresh `new`, `move local`, an owned-returning call.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, owned_sources_still_accepted) {
    auto r = runAot(std::string(kPre5236) + kProp5236 + R"BZL(
public class Program
  public static method Main(): int
    using var b := new B();
    for var i in 0..<3 do b.Val := new Item(i + 1); end;
    var l := new Item(10);
    b.Val := move l;
    b.Val := Maker.Make(20);
    Console.WriteLine(b.acc.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "36\n");
}

// Accept control: indexer `set(owned)` store of a fresh value.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, indexer_fresh_value_accepted) {
    auto r = runAot(R"BZL(
module M;
public class Item
  public tag: int;
  public constructor(t: int) tag := t; end constructor;
end Item;
public class Bag
  public acc: int;
  public owned keep: Item?;
  public property [i: int]: Item
    get => keep!;
    set(owned) => do acc := acc + value.tag; end;
  end;
end Bag;
public class Program
  public static method Main(): int
    using var g := new Bag();
    g[0] := new Item(5);
    Console.WriteLine(g.acc.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// Change-2 pin: a property read passed straight into an `owned` parameter.
// §28.3 producers table, "Property access `obj.P` (instance getter) |
// Borrowed (source: `obj`)", and A4 (v0.313): accessor read faces are
// lend-only, so the read is a borrow and §28.8 rejects it at an `owned` sink.
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, property_read_into_owned_param_rejected) {
    expectOwnedSinkReject5236(std::string(kPre5236) + kProp5236 + R"BZL(
public class Program
  public static method Take(owned x: Item) end Take;
  public static method Feed(b: B) Take(b.Val); end Feed;
  public static method Main(): int return 0; end Main;
end Program;
)BZL");
}

// Accept control for the same pin: a value-typed (int) property read is a
// copy, not a borrowable reference, so passing it on still compiles and runs
// (§28.3 producers table, A4, v0.313: the rule concerns borrowable types;
// `owned` is not admissible on a primitive parameter, §28.1, so a plain one).
BEZEL_TEST(set_owned_accessor_borrow_sink_5236, value_typed_property_read_into_param_accepted) {
    auto r = runAot(R"BZL(
module M;
public class B
  public n: int := 7;
  public property Val: int
    get => n;
  end Val;
end B;
public class Program
  public static method Take(x: int): int return x + 1; end Take;
  public static method Main(): int
    using var b := new B();
    Console.WriteLine(Take(b.Val).ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\n");
}
