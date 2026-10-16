// Spec-derived regression tests for #6232 — §35.3, §35.11 (v0.338).
//
//   §35.3: "Construction is qualified ... `FetchOutcome.NotModified` for a
//   payload-free case (a value expression, not a call)".
//   §35.11: "Classification is evaluated **per instantiation**".
//
// `InnerU<Own>.No` is a value of type `InnerU<Own>`. `resolveMemberAccess`
// typed it as the declaration `InnerU` with no type arguments (the payload
// spelling `InnerU<Own>.Has(...)` is typed by the call path and kept them),
// so a local inferred from a payload-free case was not an instantiation at
// all: it was never enrolled for disposal (a later whole-value assignment
// into it leaked), its `owned` payload slot was lowered at the unsubstituted
// parameter type (malformed IR), and once `InnerU<Own>` was classified owning
// (#6048, #6166) assigning a payload case over it was rejected as dropping an
// owned value.
//
// Every run row uses the checked oracle (`runAotChecked`, ASan/LSan), loops
// 200 times and asserts stdout.
//
// per §35.3, §35.11, §35.4, §15.3, v0.338

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* const kPrelude = R"(module M;
class Stats public static Drops: int := 0; end Stats;
class Disp
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor Stats.Drops := Stats.Drops + 1; end destructor;
end Disp;
choice Own Held(owned r: Disp), Empty; end Own;
choice Inner<T> Has(owned v: T), No; end Inner;
choice InnerU<T> Has(v: T), No; end InnerU;
choice W1<T> Ok(v: InnerU<T>), No; end W1;
)";

// A `Step(k)` whose body is `body`, called 200 times; expects
// `200 drops=<drops>`.
void expectRun(const std::string& body, const char* drops) {
    JitResult r = runAotChecked(std::string(kPrelude) + R"(class Program
  static method Step(k: int): int
)" + body + R"(
    return 1;
  end Step;
  public static method Main(): int
    var total := 0;
    for var k in 0..<200 do total := total + Program.Step(k); end;
    Console.WriteLine(total.ToString() + " drops=" + Stats.Drops.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("200 drops=") + drops + "\n");
}

}  // namespace

// `Inner<Disp>.No` then a payload case assigned over it. Before the fix:
// compiler-internal error, "Both operands to ICmp instruction are not of the
// same type! icmp eq i32 %ocd.owned.field, ptr null".
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           marked_generic_payload_free_case_then_payload_case_assigned_compiles_spec_35_11) {
    expectRun(R"(    var w1 := Inner<Disp>.No;
    w1 := Inner<Disp>.Has(new Disp(k));)", "200");
}

// `InnerU<Own>.No`, then another binding moved over it. Before the fix: runs,
// `drops=0`, 200 leaked objects.
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           unmarked_generic_payload_free_case_then_binding_assigned_disposes_spec_35_11) {
    expectRun(R"(    var w1 := InnerU<Own>.No;
    var w2 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    w1 := w2;)", "200");
}

// A payload case built from a temporary assigned over the payload-free one.
// Before the fix: leaked (and, with `InnerU<Own>` classified owning, rejected:
// "owned value in `w1` would be dropped here without disposal").
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           unmarked_generic_payload_free_case_then_temporary_payload_case_assigned_spec_35_11) {
    expectRun(R"(    var w1 := InnerU<Own>.No;
    w1 := InnerU<Own>.Has(Own.Held(new Disp(k)));)", "200");
}

// The same with the payload moved from a named binding — the family cell
// `reassign` at `InnerU<Own>`.
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           unmarked_generic_payload_free_case_then_payload_case_from_binding_assigned_spec_35_11) {
    expectRun(R"(    var w1 := InnerU<Own>.No;
    var w0 := Own.Held(new Disp(k));
    w1 := InnerU<Own>.Has(w0);)", "200");
}

// A declared local type does not hide the defect: the initializer's own type
// is what the store is checked against.
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           typed_local_initialized_by_payload_free_generic_case_then_assigned_spec_35_11) {
    expectRun(R"(    var w1: InnerU<Own> := InnerU<Own>.No;
    w1 := InnerU<Own>.Has(Own.Held(new Disp(k)));)", "200");
}

// `reassign` at `InnerU<InnerU<Own>>`.
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           nested_instantiation_payload_free_case_then_assigned_spec_35_11) {
    expectRun(R"(    var w1 := InnerU<InnerU<Own>>.No;
    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    w1 := InnerU<InnerU<Own>>.Has(w0);)", "200");
}

// `reassign` at `W1<Own>` (declared payload `InnerU<T>`).
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           declared_payload_instantiation_payload_free_case_then_assigned_spec_35_11) {
    expectRun(R"(    var w1 := W1<Own>.No;
    var w0 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    w1 := W1<Own>.Ok(w0);)", "200");
}

// Control: the payload-free value on its own owns nothing.
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           payload_free_generic_case_alone_is_disposable_and_empty_spec_35_11) {
    expectRun(R"(    var w1 := InnerU<Own>.No;)", "0");
}

// Control, the other direction: `InnerU<int>.No` is value-like and copies
// freely, before and after.
BEZEL_TEST(generic_payload_free_case_type_args_6232,
           value_like_instantiation_payload_free_case_stays_copyable_spec_35_11) {
    expectRun(R"(    var w1 := InnerU<int>.No;
    var w2 := w1;
    var w3 := w1;
    w1 := InnerU<int>.Has(k);)", "0");
}
