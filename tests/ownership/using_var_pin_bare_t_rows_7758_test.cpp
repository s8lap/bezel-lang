// #7758 follow-up rows (review F2, spec §28.10 / K8.4 / §28.1 clause 3,
// v0.357): the K8.4 temporary boundary for `using var x := <temp>.V` at a
// bare type parameter T. Rejecting shapes: unconstrained T, enclosing-type
// `T: class`, nullable field, call-produced box, if-arm temporary. Accepting
// twins: a named (non-temporary) box, a parameter box's field. (`T: value` temp: #8456.)
//
// per §28.10, K8.4, §28.1, v0.357

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
    "public class Box<T> "
    "  public owned V: T; "
    "  public constructor(owned v: T) V := v; end constructor; "
    "end Box; "
    "public class BoxN<T> "
    "  public owned V: T? := null; "
    "  public constructor(owned v: T) V := v; end constructor; "
    "end BoxN; "
    "public class MkBox "
    "  public static method Mk<T>(owned t: T): owned Box<T> where T: class "
    "    return new Box<T>(move t); "
    "  end Mk; "
    "end MkBox; ";

std::string withGen(const std::string& gen, const std::string& mainBody) {
    return std::string("module M; ") + kDecls + gen +
           "public class Program "
           "  public static method Main(): int " +
           mainBody +
           "  end Main; "
           "end Program;";
}

void expectK84Rejected(const std::string& gen, const std::string& mainBody) {
    auto r = runAotChecked(withGen(gen, mainBody));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(K8.4"));
}
}  // namespace

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_unconstrained_t_temp_field_rejected) {
    expectK84Rejected(
        "public class GenP "
        "  public static method G<T>(owned t: T): int "
        "    using var x := new Box<T>(move t).V; return 1; "
        "  end G; "
        "end GenP; ",
        "return GenP.G<Res>(new Res()) - 1; ");
}

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_enclosing_type_t_temp_field_rejected) {
    expectK84Rejected(
        "public class GenQ<T> where T: class "
        "  public static method G(owned t: T): int "
        "    using var x := new Box<T>(move t).V; return 1; "
        "  end G; "
        "end GenQ; ",
        "return GenQ<Res>.G(new Res()) - 1; ");
}

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_nullable_field_of_temp_rejected) {
    expectK84Rejected(
        "public class GenP "
        "  public static method G<T>(owned t: T): int where T: class "
        "    using var x := new BoxN<T>(move t).V; return 1; "
        "  end G; "
        "end GenP; ",
        "return GenP.G<Res>(new Res()) - 1; ");
}

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_call_produced_box_field_rejected) {
    expectK84Rejected(
        "public class GenP "
        "  public static method G<T>(owned t: T): int where T: class "
        "    using var x := MkBox.Mk<T>(move t).V; return 1; "
        "  end G; "
        "end GenP; ",
        "return GenP.G<Res>(new Res()) - 1; ");
}

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_if_arm_temp_field_rejected) {
    expectK84Rejected(
        "public class GenP "
        "  public static method G<T>(owned t: T, c: bool): int where T: class "
        "    using var x := (if c then new Box<T>(move t).V else null); "
        "    return 1; "
        "  end G; "
        "end GenP; ",
        "return GenP.G<Res>(new Res(), true) - 1; ");
}

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_named_box_field_pin_accepted) {
    auto r = runAotChecked(withGen(
        "public class GenP "
        "  public static method G<T>(owned t: T): int where T: class "
        "    var b := new Box<T>(move t); "
        "    using var x := b.V; return 1; "
        "  end G; "
        "end GenP; ",
        "return GenP.G<Res>(new Res()) - 1; "));
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}

BEZEL_TEST(ownership_using_var_pin_7758_rows,
           spec_k8_4_param_box_field_pin_accepted) {
    auto r = runAotChecked(withGen(
        "public class GenP "
        "  public static method G<T>(b: Box<T>): int where T: class "
        "    using var x := b.V; return 1; "
        "  end G; "
        "end GenP; ",
        "var b := new Box<Res>(new Res()); return GenP.G<Res>(b) - 1; "));
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}
