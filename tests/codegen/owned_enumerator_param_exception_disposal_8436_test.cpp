// Codegen — issue #8436: an `owned e: IEnumerator<T>` PARAMETER disposed on
// the EXCEPTION edge runs the generator's `finally` (spec §28.3 K3.15 edge
// parity / §28.3 K3.14 affinity / §29.7, v0.357).
//
// THE DEFECT (measured on staging r30-c1 @ e41796df3, which carries the #7961
// parameter arm). The caller hands a fresh enumerator to an `owned` parameter;
// §28.1 makes the CALLEE its owner. On the NORMAL edge the callee's disposal
// went through the iterator routine (vtable slot 1, §29.7) and the generator's
// `finally` ran. On the EXCEPTION edge the SAME parameter was disposed by the
// generic `unwindOnly` arm of `Emitter::emitDisposals`, which dispatches the
// CLASS convention (§28.2: Dispose at vtable slot 2, then reclaim the block).
// For an iterator-format enumerator slot 2 is MoveNext, not Dispose: the
// generator was RESUMED and its frame then freed, and the pending `finally`
// never ran (§29.7). MEASURED in the compiled artifact: `bz_wait_for_borrows(e)`
// then a call through `e->vtable + 0x10` (slot 2) then `free(e)`.
//
// The `unwindOnly` flag is what separated the two edges: the ownership pass
// drops a value that reaches the method exit ONLY via a throw from the exit
// state (§28.3's exception-path discharge), so the §28.3 K3.14 auto-drop is
// not armed and the unwind edge is the only disposal the parameter has.
//
// Oracle: runAotChecked (ASan+LSan) plus stdout — the `finally` must print
// exactly once per iteration, on the exception edge, and the frame must be
// reclaimed (no leak, no double free).
//
// Spec: §28.1, §28.3 (K3.14 affinity, K3.15 edge parity), §29.7.
// Spec revision v0.357.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The issue's own generator (issue #8436, probe mD2.bzl): a `finally` that
// prints "fin" when the enumerator is disposed.
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

// THE ISSUE'S REPRO (mD2.bzl), 100x.
//
// The callee advances the enumerator once and then throws. The exception
// unwinds out of the callee, so §28.3 K3.15 (edge parity) requires the
// parameter's implicit disposal to be the SAME one the normal edge performs —
// the iterator disposal of §29.7, which runs the generator's pending `finally`
// and reclaims the frame.
//
// Before the fix: "thrown", "caught" — the `finally` never ran (the frame was
// freed by the class convention's slot-2 dispatch).
// After the fix:  "thrown", "fin", "caught".
BEZEL_TEST(codegen_owned_enumerator_param_exception_8436,
           issue_shape_exception_edge_runs_generator_finally_spec_29_7) {
    std::string src =
        std::string("module M; ") + kGen +
        "class Y "
        "  public static method Take(owned e: IEnumerator<int>) "
        "    var ok := e.MoveNext(); "
        "    Console.WriteLine(\"thrown\"); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Take; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    try "
        "      Y.Take(G.Gen().GetEnumerator()); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("thrown\nfin\ncaught\n", 100) + "end\n");
}

// CONTROL — the NORMAL edge of the same parameter (the #7961 arm's own row).
// The callee returns normally, so the §28.3 K3.14 auto-drop fires at the
// method's end and the iterator routine runs the `finally`. This row is GREEN
// on the base and on the fix: it pins that the fix did not disturb the normal
// edge, and it is the admitting twin of the raise row above (the same
// parameter shape, a legal outcome).
BEZEL_TEST(codegen_owned_enumerator_param_exception_8436,
           control_normal_edge_runs_generator_finally_spec_28_3) {
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

// THE IOwningEnumerator TWIN — the same exception edge at the OTHER
// declaration-less sync iterator-format head (§29.1's owning enumeration
// interfaces; `common::isSyncIterFormatEnumeratorHead` admits exactly
// "IEnumerator" and "IOwningEnumerator"). The generator yields an owned class
// value, so the enumerator is an `IOwningEnumerator<R>`; the parameter is
// `owned e: IOwningEnumerator<R>` and the same unwind edge applies.
//
// Before the fix: the `finally` never ran. After the fix: it runs once.
BEZEL_TEST(codegen_owned_enumerator_param_exception_8436,
           issue_shape_owning_enumerator_exception_edge_spec_29_7) {
    std::string src =
        "module M; "
        "class R "
        "  private n: int := 1; "
        "end R; "
        "class G "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    try yield return new R(); "
        "    finally Console.WriteLine(\"fin\"); "
        "    end; "
        "  end Gen; "
        "end G; "
        "class Y "
        "  public static method Take(owned e: IOwningEnumerator<R>) "
        "    var ok := e.MoveNext(); "
        "    Console.WriteLine(\"thrown\"); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Take; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    try "
        "      Y.Take(G.Gen().GetOwningEnumerator()); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("thrown\nfin\ncaught\n", 100) + "end\n");
}

// CONTROL — a LOCAL enumerator on the same exception edge (issue #8436's
// probe mDL.bzl). The local's disposal was already on the iterator routine,
// so the `finally` ran before the fix too: GREEN on the base and on the fix.
// It is the positive control that isolates the defect to the PARAMETER's
// unwind-only registration.
BEZEL_TEST(codegen_owned_enumerator_param_exception_8436,
           control_local_enumerator_exception_edge_spec_29_7) {
    std::string src =
        std::string("module M; ") + kGen +
        "class Y "
        "  public static method Take() "
        "    var e := G.Gen().GetEnumerator(); "
        "    var ok := e.MoveNext(); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Take; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    try "
        "      Y.Take(); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("fin\ncaught\n", 100) + "end\n");
}

// CONTROL — a RAW USER CLASS at the `IEnumerator<int>` head, on the EXCEPTION
// edge. A hand-written class implementing `IEnumerator<int>` reaches the
// `owned` parameter as a class instance (class vtable, Dispose at slot 2,
// §28.2), not as an iterator frame (slot 1, §29.11). The disposal must still
// reach the class's own `Dispose()` on the unwind edge — the runtime
// discriminator (§29.2's slot-0 self-reference) picks the class arm.
//
// GREEN on the base and on the fix: before the fix the generic unwind arm
// dispatched slot 2 and the class's Dispose ran; after the fix the iterator
// arm's class branch dispatches the same slot 2. This row pins that the fix
// did not turn the class case into a slot-1 dispatch (which would SEGV, as
// #7961 measured).
BEZEL_TEST(codegen_owned_enumerator_param_exception_8436,
           control_raw_user_class_enumerator_exception_edge_spec_28_2) {
    std::string src =
        "module M; "
        "class MyE: IEnumerator<int> "
        "  private i: int := 0; "
        "  public method MoveNext(): bool "
        "    if this.i < 2 then this.i := this.i + 1; return true; end if; "
        "    return false; "
        "  end MoveNext; "
        "  public property Current: int get => this.i; end Current; "
        "  destructor "
        "    Console.WriteLine(\"mydisp\"); "
        "  end destructor; "
        "end MyE; "
        "class Y "
        "  public static method Take(owned e: IEnumerator<int>) "
        "    var ok := e.MoveNext(); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Take; "
        "end Y; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    try "
        "      Y.Take(new MyE()); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("mydisp\ncaught\n", 100) + "end\n");
}
