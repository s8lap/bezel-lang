// Codegen test for #8803 (§28.14, v0.359): the newly admitted
// same-bounding-object forward through a constructor chain runs correctly
// under the checked oracle. A class-typed `scoped` borrow parameter forwarded
// by `base(s)` into the base's `scoped` borrow formal and stored there is read
// back through the constructed object while its source is still alive
// (§28.14 *scoped construction*: the source, declared before the bounding
// `using var`, outlives the bounding object). RED on the base: the program was
// rejected at compile time.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

BEZEL_TEST(ctor_chain_scoped_borrow_forward_8803,
           base_chain_scoped_borrow_forward_runs_200x_spec_28_14) {
    auto r = runAotChecked(
        "module T;\n"
        "public class Box\n"
        "  public v: int;\n"
        "  public constructor(x: int) v := x; end;\n"
        "end;\n"
        "public class B\n"
        "  private src: Box;\n"
        "  public constructor(scoped s: Box) src := s; end;\n"
        "  public method Get(): int => src.v;\n"
        "end;\n"
        "public class D : B\n"
        "  public constructor(scoped s: Box) base(s); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(): int\n"
        "    var i := 0; var tot := 0;\n"
        "    while i < 200 do\n"
        "      using var bx := new Box(i);\n"
        "      using var k := new D(bx);\n"
        "      tot := tot + k.Get() + 1;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.Write(tot);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20100"));
}
