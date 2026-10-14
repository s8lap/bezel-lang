// Codegen — §12.1 / §27.8 (v0.338): overloads that differ only in generic
// ARGUMENTS must get distinct vtable slots.
//
// Pre-fix (src/codegen/LlvmBackend_impl.hpp, Emitter::indexerSlotKey and
// Emitter::methodSlotKey), a parameter was keyed by its head name only, so
// `Array<int>` and `Array<string>` collided. A base-typed call to the
// `Array<string>` overload dispatched the `Array<int>` override: the indexer
// printed `2` twice (expected `2` then `5`), the method printed `20` twice
// (expected `20` then `11`). The alias spellings (`type AI := Array<int>`)
// collided the same way.
//
// Rows (runAot; the observable is captured stdout):
//   - indexer, non-alias spelling (`[k: Array<int>]` / `[k: Array<string>]`)
//   - method, non-alias spelling (`Foo(x: Array<int>)` / `Foo(x: Array<string>)`)
//   - indexer, alias spelling on the override (`[k: AI]`)
//   - method, alias spelling on the override (`Foo(x: L)`)
//   - control: a single non-generic overload, unchanged

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// §12.1 v0.338: indexer overloads over Array<int> and Array<string>. Pre-fix
// stdout is "2\n2\n".
BEZEL_TEST(codegen_generic_argument_slot_key_6074,
           indexer_generic_argument_overloads_do_not_collide_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "public class B "
        "  public virtual property [k: Array<int>]: int "
        "    get => 1; "
        "  end; "
        "  public virtual property [k: Array<string>]: int "
        "    get => 5; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [k: Array<int>]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    var a := new Array<int>(1); "
        "    var s := new Array<string>(1); "
        "    Console.WriteLine(b[a].ToString()); "
        "    Console.WriteLine(b[s].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n5\n"));
}

// §12.1 v0.338: method overloads over Array<int> and Array<string>. Pre-fix
// stdout is "20\n20\n".
BEZEL_TEST(codegen_generic_argument_slot_key_6074,
           method_generic_argument_overloads_do_not_collide_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "public class B "
        "  public virtual method Foo(x: Array<int>): int "
        "    return 10; "
        "  end Foo; "
        "  public virtual method Foo(x: Array<string>): int "
        "    return 11; "
        "  end Foo; "
        "end B; "
        "public class Dv: B "
        "  public override method Foo(x: Array<int>): int "
        "    return 20; "
        "  end Foo; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    var a := new Array<int>(1); "
        "    var s := new Array<string>(1); "
        "    Console.WriteLine(b.Foo(a).ToString()); "
        "    Console.WriteLine(b.Foo(s).ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "20\n11\n"));
}

// §27.8 + §12.1 v0.338: the alias-spelled override indexer `[k: AI]` is
// `Array<int>`, so the `Array<string>` overload keeps its own slot.
BEZEL_TEST(codegen_generic_argument_slot_key_6074,
           indexer_alias_override_generic_argument_overloads_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "type AI := Array<int>; "
        "public class B "
        "  public virtual property [k: Array<int>]: int "
        "    get => 1; "
        "  end; "
        "  public virtual property [k: Array<string>]: int "
        "    get => 5; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [k: AI]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    var a := new Array<int>(1); "
        "    var s := new Array<string>(1); "
        "    Console.WriteLine(b[a].ToString()); "
        "    Console.WriteLine(b[s].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n5\n"));
}

// §27.8 + §12.1 v0.338: the alias-spelled override method `Foo(x: L)` is
// `Array<int>`, so the `Array<string>` overload keeps its own slot.
BEZEL_TEST(codegen_generic_argument_slot_key_6074,
           method_alias_override_generic_argument_overloads_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "type L := Array<int>; "
        "public class B "
        "  public virtual method Foo(x: Array<int>): int "
        "    return 10; "
        "  end Foo; "
        "  public virtual method Foo(x: Array<string>): int "
        "    return 11; "
        "  end Foo; "
        "end B; "
        "public class Dv: B "
        "  public override method Foo(x: L): int "
        "    return 20; "
        "  end Foo; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    var a := new Array<int>(1); "
        "    var s := new Array<string>(1); "
        "    Console.WriteLine(b.Foo(a).ToString()); "
        "    Console.WriteLine(b.Foo(s).ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "20\n11\n"));
}

// Control: a single non-generic overload dispatches to the override, unchanged.
BEZEL_TEST(codegen_generic_argument_slot_key_6074,
           control_non_generic_override_dispatches_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "public class B "
        "  public virtual method Foo(x: int): int "
        "    return 10; "
        "  end Foo; "
        "end B; "
        "public class Dv: B "
        "  public override method Foo(x: int): int "
        "    return 20; "
        "  end Foo; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b.Foo(3).ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "20\n"));
}
