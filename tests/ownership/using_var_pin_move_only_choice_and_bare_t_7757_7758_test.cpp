// #7757: a `using var` pin of a MOVE-ONLY CHOICE field freezes the holder
// (spec §28.10 *`using var` initialized from a borrow* and the consume-site
// freeze, §28.3 K5.5, v0.357): `using var x := h.F; var h2 := move h;` is a
// compile error exactly as for a class field.
//
// #7758: `using var x := new Box<T>(move t).V` in a generic body with
// `T: class` is rejected (K8.4, §28.10, §28.1 clause 3, v0.357): the bare type
// parameter is read as a class, so the pin borrows from a temporary that is
// disposed at the statement's end.
//
// per §28.10, §28.3 K5.5, K8.4, §28.1, v0.357

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
    "public choice OC Text(owned r: Res), Empty; end OC; "
    "public class Box<T> "
    "  public owned V: T; "
    "  public constructor(owned v: T) V := v; end constructor; "
    "end Box; "
    "public class H "
    "  public owned C: Res := new Res(); "
    "  public owned F: OC := OC.Text(new Res()); "
    "  public constructor() end constructor; "
    "end H; ";
}  // namespace

BEZEL_TEST(ownership_using_var_pin_7757,
           spec_28_10_pin_of_a_move_only_choice_field_freezes_the_holder) {
    auto r = runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); using var x := h.F; var h2 := move h; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "consume-site freeze"));
}

BEZEL_TEST(ownership_using_var_pin_7757,
           spec_28_10_class_field_pin_control_still_rejected) {
    auto r = runAotChecked(
        std::string("module M; ") + kDecls +
        "public class Program "
        "  public static method Main(): int "
        "    var h := new H(); using var x := h.C; var h2 := move h; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "consume-site freeze"));
}
