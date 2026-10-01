// Codegen — §27.6 (v0.313) / #5237: a bound method reference over a RECORD
// receiver (`var m: () -> string := r.Show;`). §27.6: "instance methods (the
// instance is captured at formation — by reference for class-typed receivers,
// **by copy** for value-shaped (record) receivers ...)". The env block holds
// the record's VALUE; the thunk handed the method's `this` the field payload
// as if it were a pointer (SEGV at the first field's value, e.g. 0x3).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

void expectOut(const std::string& decls, const std::string& mainBody,
               const std::string& expected) {
    std::string src = "module M; " + decls +
        "class C public static method Main(): int " + mainBody +
        " return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

const char* kR =
    "interface IShow method Show(): string; end IShow; "
    "record R(A: int) : IShow "
    "  public method Show(): string return $\"R{A}\"; end Show; "
    "end R; ";

}  // namespace

// per §27.6 (v0.313), issue #5237 repro.
BEZEL_TEST(bound_method_ref_record_receiver_5237, repro_single_int_field) {
    expectOut(kR, "var r := new R(3); var m: () -> string := r.Show; "
                  "Console.WriteLine(m());", "R3\n");
}

BEZEL_TEST(bound_method_ref_record_receiver_5237, two_field_record) {
    expectOut("record P(X: int, Y: int) "
              "  public method Sum(): int return X * 10 + Y; end Sum; "
              "end P; ",
              "var p := new P(4, 7); var m: () -> int := p.Sum; "
              "Console.WriteLine(m());", "47\n");
}

BEZEL_TEST(bound_method_ref_record_receiver_5237, string_field_record) {
    expectOut("record N(Name: string, Age: int) "
              "  public method Desc(): string return $\"{Name}:{Age}\"; end Desc; "
              "end N; ",
              "var n := new N(\"ann\", 30); var m: () -> string := n.Desc; "
              "Console.WriteLine(m());", "ann:30\n");
}

// Receiver static type is the record; the method implements IShow.
BEZEL_TEST(bound_method_ref_record_receiver_5237, interface_method_via_record) {
    expectOut(kR, "var r := new R(5); var m: () -> string := r.Show; "
                  "var i: IShow := r; "
                  "Console.WriteLine(m()); Console.WriteLine(i.Show());",
              "R5\nR5\n");
}

// per §27.6 (v0.313): captured BY COPY at formation, so reassigning the local
// afterwards does not change what the reference returns.
BEZEL_TEST(bound_method_ref_record_receiver_5237, snapshot_after_reassign) {
    expectOut(kR, "var r := new R(3); var m: () -> string := r.Show; "
                  "r := new R(9); "
                  "Console.WriteLine(m()); Console.WriteLine(r.Show());",
              "R3\nR9\n");
}

// Control: class receiver is captured by reference.
BEZEL_TEST(bound_method_ref_record_receiver_5237, class_receiver_control) {
    expectOut("class K "
              "  private v: int; "
              "  public constructor(x: int) v := x; end constructor; "
              "  public method Show(): string return $\"K{v}\"; end Show; "
              "end K; ",
              "var k := new K(7); var m: () -> string := k.Show; "
              "Console.WriteLine(m());", "K7\n");
}

// Bind + invoke 200x; a leak or mis-sized env would surface under the ASan leg.
BEZEL_TEST(bound_method_ref_record_receiver_5237, loop_200_bind_and_invoke) {
    expectOut(kR, "var total := 0; "
                  "for var i in 0..<200 do "
                  "  var r := new R(i); var m: () -> string := r.Show; "
                  "  total += m().Length; "
                  "end for; "
                  "Console.WriteLine(total);",
              "690\n");
}
