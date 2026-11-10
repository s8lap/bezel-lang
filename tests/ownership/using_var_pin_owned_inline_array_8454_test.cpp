// #8454: a `using var` pin of an OWNING INLINE-ARRAY field (`[2 owned]Res?`)
// freezes the holder (spec §15.3 owning inline arrays are move-only values;
// §28.10 *`using var` initialized from a borrow* and the consume-site freeze,
// §28.3 K5.5, v0.357): `using var x := h.Arr; var h2 := move h;` is a compile
// error exactly as for a class or move-only choice field (#7757).
//
// per §15.3, §28.10, §28.3 K5.5, v0.357

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {
const char* kDecls =
    "public class Res "
    "  public constructor() end constructor; "
    "end Res; "
    "public class H "
    "  public Arr: [2 owned]Res? := [new Res(), new Res()]; "
    "  public Arr2: [2]Res? := [null, null]; "
    "  public constructor() end constructor; "
    "end H; ";
}  // namespace

BEZEL_TEST(ownership_using_var_pin_8454,
           spec_28_10_pin_of_an_owning_inline_array_field_freezes_the_holder) {
    auto r = runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); using var x := h.Arr; var h2 := move h; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "consume-site freeze"));
}

BEZEL_TEST(ownership_using_var_pin_8454,
           spec_28_10_owning_inline_array_pin_without_move_is_accepted) {
    auto r = runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); using var x := h.Arr; return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}
