// Codegen — issue #7961: an `owned e: IEnumerator<T>` PARAMETER is disposed by
// the callee (spec §28.1 clause 1 / §28.3 / §28.6 / §29.7, v0.357).
//
// THE DEFECT (measured on staging e49c32aca). The caller hands a fresh
// enumerator to an `owned` parameter; §28.1 makes the CALLEE its owner, and the
// caller's argument is moved-from. registerUnwindOwnedParam had no arm for the
// declaration-less `IEnumerator<T>` head, so the callee registered no disposal:
// the enumerator frame leaked (40 B per call) and the generator's `finally`
// ("fin") never ran (§29.7).
//
// Oracle: runAotChecked (ASan+LSan) plus stdout (the `finally` count and the
// order "fin" before the caller's next print).
//
// Spec: §28.1 clause 1, §28.3, §28.6, §29.7. Spec revision v0.357.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kGen =
    "class G "
    "  public static method Gen(): IEnumerable<int> "
    "    try yield return 1; yield return 2; "
    "    finally Console.WriteLine(\"fin\"); end; "
    "  end Gen; "
    "end G; ";

std::string rep(const std::string& unit, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += unit;
    return out;
}

}  // namespace

// The issue's minimal repro (7961L): the callee only calls MoveNext; the
// compiler-inserted scope-end disposal must run the `finally` once per call.
BEZEL_TEST(codegen_owned_enumerator_param_7961,
           issue_shape_callee_never_disposes_spec_28_1) {
    std::string src =
        std::string("module M; ") + kGen +
        "class Y "
        "  public static method TakeNoDispose(owned e: IEnumerator<int>) "
        "    var ok := e.MoveNext(); "
        "  end TakeNoDispose; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    Y.TakeNoDispose(G.Gen().GetEnumerator()); "
        "    Console.WriteLine(\"closed\"); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\nclosed\n", 100) + "end\n");
}

// NO DOUBLE-FREE twin: a callee that discharges the parameter itself with an
// explicit `Dispose()` must still dispose it exactly once (the scope-end drop
// is only for a never-discharged owner, §28.3 K3.14).
BEZEL_TEST(codegen_owned_enumerator_param_7961,
           control_callee_disposes_explicitly_spec_28_3) {
    std::string src =
        std::string("module M; ") + kGen +
        "class Y "
        "  public static method TakeAndDispose(owned e: IEnumerator<int>) "
        "    var ok := e.MoveNext(); "
        "    e.Dispose(); "
        "  end TakeAndDispose; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    Y.TakeAndDispose(G.Gen().GetEnumerator()); "
        "    Console.WriteLine(\"closed\"); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\nclosed\n", 100) + "end\n");
}

// NO DOUBLE-FREE twin: a callee that MOVES the parameter on to another owner
// (§28.8.1) must not also dispose it; the second callee disposes it once.
BEZEL_TEST(codegen_owned_enumerator_param_7961,
           control_callee_forwards_by_move_spec_28_8_1) {
    std::string src =
        std::string("module M; ") + kGen +
        "class Y "
        "  public static method Sink(owned e: IEnumerator<int>) "
        "    var ok := e.MoveNext(); "
        "  end Sink; "
        "  public static method Forward(owned e: IEnumerator<int>) "
        "    Y.Sink(move e); "
        "  end Forward; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    Y.Forward(G.Gen().GetEnumerator()); "
        "    Console.WriteLine(\"closed\"); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\nclosed\n", 100) + "end\n");
}

// RAW USER CLASS at the head: a hand-written class implementing
// `IEnumerator<int>` reaches the `owned` parameter as a class instance (class
// vtable, Dispose at slot 2, §28.2), NOT as an iterator frame (slot 1,
// §29.11). The callee's disposal must tell the two apart by the VALUE (§29.2);
// a blind slot-1 dispatch SEGVs in `itmap$M.MyE` (MEASURED during the fix).
// The pre-fix compiler leaked this shape without crashing; the row pins that
// the fix does not trade the leak for a crash.
BEZEL_TEST(codegen_owned_enumerator_param_7961,
           control_raw_user_class_enumerator_spec_28_2) {
    std::string src =
        "module M; "
        "class MyE: IEnumerator<int> "
        "  private i: int := 0; "
        "  public method MoveNext(): bool "
        "    if this.i < 2 then this.i := this.i + 1; return true; end if; "
        "    return false; "
        "  end MoveNext; "
        "  public property Current: int get => this.i; end Current; "
        "end MyE; "
        "class Y "
        "  public static method Take(owned e: IEnumerator<int>) "
        "    var ok := e.MoveNext(); "
        "  end Take; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    Y.Take(new MyE()); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "end\n");
}
