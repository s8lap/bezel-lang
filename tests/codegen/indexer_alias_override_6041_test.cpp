// Codegen — §27.8 / §12.1 (v0.338): a type alias is a pure synonym, so an
// indexer override whose index parameter is spelled through an alias must
// override the base indexer, not get a new vtable slot.
//
// Pre-fix (src/codegen/LlvmBackend_impl.hpp, Emitter::indexerSlotKey), the
// AST branch spelled the index parameter's syntactic name (`EA`), so
// `[e: EA]` on the base and `[e: Exception]` on the override produced
// different slot keys. The override then got its own slot and a read through
// a base-typed receiver dispatched the base body (prints 1, expected 2).
//
// Rows (runAot; the observable is captured stdout):
//   - alias on the base, concrete type on the override (issue repro)
//   - alias on the override, concrete type on the base
//   - alias of an alias (`type EB := EA`) on the base
//   - generic alias target (`type AI := Array<int>`) on the base
//   - control: no alias anywhere, override dispatches

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// §27.8 / §12.1 v0.338: alias on the base indexer. Pre-fix prints `1`.
BEZEL_TEST(codegen_indexer_alias_override_6041,
           alias_on_base_override_dispatches_27_8_v0338) {
    auto r = runAot(
        "module Al; "
        "type EA := Exception; "
        "public class B "
        "  public virtual property [e: EA]: int "
        "    get => 1; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [e: Exception]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b[new Exception()].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n"));
    BEZEL_EXPECT_FALSE(contains(r.stdoutText, "1\n"));
}

// §27.8 / §12.1 v0.338: alias on the override indexer, concrete base.
BEZEL_TEST(codegen_indexer_alias_override_6041,
           alias_on_override_dispatches_27_8_v0338) {
    auto r = runAot(
        "module Al; "
        "type EA := Exception; "
        "public class B "
        "  public virtual property [e: Exception]: int "
        "    get => 1; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [e: EA]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b[new Exception()].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n"));
    BEZEL_EXPECT_FALSE(contains(r.stdoutText, "1\n"));
}

// §27.8 v0.338: alias of an alias on the base indexer. The chained head
// resolves to Exception.
BEZEL_TEST(codegen_indexer_alias_override_6041,
           alias_of_alias_dispatches_27_8_v0338) {
    auto r = runAot(
        "module Al; "
        "type EA := Exception; "
        "type EB := EA; "
        "public class B "
        "  public virtual property [e: EB]: int "
        "    get => 1; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [e: Exception]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b[new Exception()].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n"));
    BEZEL_EXPECT_FALSE(contains(r.stdoutText, "1\n"));
}

// §27.8 v0.338: alias whose target is a generic instantiation.
BEZEL_TEST(codegen_indexer_alias_override_6041,
           generic_alias_dispatches_27_8_v0338) {
    auto r = runAot(
        "module Al; "
        "type AI := Array<int>; "
        "public class B "
        "  public virtual property [a: AI]: int "
        "    get => 1; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [a: Array<int>]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b[new Array<int>()].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n"));
    BEZEL_EXPECT_FALSE(contains(r.stdoutText, "1\n"));
}

// Control: no alias anywhere; the override dispatches. Unchanged by the fix.
BEZEL_TEST(codegen_indexer_alias_override_6041,
           control_no_alias_override_dispatches_27_8_v0338) {
    auto r = runAot(
        "module Al; "
        "public class B "
        "  public virtual property [e: Exception]: int "
        "    get => 1; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [e: Exception]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b[new Exception()].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "2\n"));
}
