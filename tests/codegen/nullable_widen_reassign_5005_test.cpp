// Regression tests for issue #5005 — §5.1 "Nullable conversions" (`S`
// converts to `T?` via `S → T → T?`) at a REASSIGNMENT (`x := k;` to an
// EXISTING nullable-value local/field/element), v0.307.
//
// `AssignmentStmt` lowering (LlvmBackend_Stmts.cpp, the shared `:=` /
// `??=` coercion preamble reached by the local-slot arm and the generic
// `lvalueAddr` fallback arm) called `coerceNumericToType(rhs, elemTy, …)`
// with the WHOLE nullable-value carrier type `{i8 hasValue, T payload}` as
// the widen target, instead of the payload type `T`. Every one of
// `coerceNumericToType`'s §5.1 arms declines for a struct target (neither
// integer nor float), so a narrower RHS (e.g. an `i32` at a `long?` slot's
// `i64` payload) reached the CG-nullable wrap block below still at its
// original width; that block's `rhs->getType() == st->getElementType(1)`
// exact-match check then ALSO failed, so the store fell through UNWRAPPED —
// a bare `i32` stored at the struct's address overwrites only the leading
// `hasValue` byte (plus padding), leaving the payload bytes at their prior
// value (0 for a freshly-`null`-initialized slot). MEASURED (issue #5005):
// `x := k;` (`k: int`, `x: long?`) printed `0` instead of the widened `4`.
//
// The declaration-initializer form (`var w: long? := k;`) was already
// correct (fix #839 widens against the payload there via `slotPayloadTy`).
// This fix mirrors that for REASSIGNMENT: `coerceNumericToType(rhs,
// slotPayloadTy(elemTy), …)` in the shared `:=` preamble, the analogous
// widen in the `??=` short-circuit arm's own coercion block (same file,
// same bug pattern, same fix), the `coerceToNullableSlot` helper
// (LlvmBackend_impl.hpp, used by inline-array-element stores among other
// call sites) gaining an optional widen-before-wrap step, and the heap
// `Array<T>` / `Dictionary<K,V>` indexer-set paths in LlvmBackend_Assign.cpp
// gaining the same widen before `wrapNullableValue` (pre-fix these crashed
// the compiler with "Invalid InsertValueInst operands!" — an IR
// verification failure — rather than printing a wrong value).
//
// spec §5.1 "Implicit conversions" and "Nullable conversions" (`S` converts
// to `T?` via `S → T → T?`), §7.2 (nullable value types), v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 (v0.307), fix #5005 — the issue's repro verbatim: local `:=`
// reassignment, an `if`-guarded reassignment, a `for`-loop-local
// reassignment, and the already-correct declaration-initializer control.
// Pre-fix: "a 0\nb 0\nc 0\ne 4\n"; expected (and the issue's own stated
// expected output): "a 4\nb 4\nc 3\ne 4\n".
BEZEL_TEST(nullable_widen_reassign_5005, issue_repro_verbatim) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var k: int := 4; "
        "    var x: long? := null; "
        "    x := k; "
        "    Console.WriteLine($\"a {x}\"); "
        "    var y: long? := null; "
        "    if k > 0 then y := k; end; "
        "    Console.WriteLine($\"b {y}\"); "
        "    for var j in 3..<4 do "
        "      var z: long? := null; "
        "      z := j; "
        "      Console.WriteLine($\"c {z}\"); "
        "    end; "
        "    var w: long? := k; "
        "    Console.WriteLine($\"e {w}\"); "
        "  end Main; "
        "end C;",
        "a 4\nb 4\nc 3\ne 4\n");
}

// per §5.1 (v0.307), fix #5005 — control: a same-width source (`long` at
// `long?`) was already correct pre-fix (the issue's own stated control) and
// must remain so.
BEZEL_TEST(nullable_widen_reassign_5005, control_same_width_source_unaffected) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var k: int := 4; "
        "    var kl: long := k; "
        "    var x: long? := null; "
        "    x := kl; "
        "    Console.WriteLine($\"same {x}\"); "
        "  end Main; "
        "end C;",
        "same 4\n");
}

// per §5.1 (v0.307), fix #5005 — a CLASS FIELD (via an auto-property backing
// field) reassigned through `this.f := k`: same shared preamble, generic
// `lvalueAddr` fallback arm.
BEZEL_TEST(nullable_widen_reassign_5005, field_reassign_this_dot_f) {
    expectPrints(
        "module M; "
        "public class C "
        "  public property F: long?; "
        "  public method Set(k: int) this.F := k; end Set; "
        "  public static method Main() "
        "    var c := new C(); c.Set(4); "
        "    Console.WriteLine($\"field {c.F}\"); "
        "  end Main; "
        "end C;",
        "field 4\n");
}

// per §5.1 (v0.307), fix #5005 — a STATIC field reassignment.
BEZEL_TEST(nullable_widen_reassign_5005, static_field_reassign) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static S: long? := null; "
        "  public static method Set(k: int) S := k; end Set; "
        "  public static method Main() "
        "    var k: int := 4; C.Set(k); "
        "    Console.WriteLine($\"static {C.S}\"); "
        "  end Main; "
        "end C;",
        "static 4\n");
}

// per §5.1 (v0.307), fix #5005 — `ref` and `out` parameters of nullable-value
// type, both reassigned from a narrower `int` inside the callee.
BEZEL_TEST(nullable_widen_reassign_5005, ref_and_out_params) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method SetRef(ref x: long?, k: int) x := k; end SetRef; "
        "  public static method SetOut(out x: long?, k: int) x := k; end SetOut; "
        "  public static method Main() "
        "    var r: long? := null; "
        "    SetRef(ref r, 4); "
        "    Console.WriteLine($\"ref {r}\"); "
        "    var o: long? := null; "
        "    SetOut(out o, 5); "
        "    Console.WriteLine($\"out {o}\"); "
        "  end Main; "
        "end C;",
        "ref 4\nout 5\n");
}

// per §5.1 (v0.307), fix #5005 — `double?` target from an `int` source, and
// `long?` target from `byte` and `short` sources.
BEZEL_TEST(nullable_widen_reassign_5005, double_and_byte_short_widen) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var i: int := 3; "
        "    var d: double? := null; "
        "    d := i; "
        "    Console.WriteLine($\"double {d}\"); "
        "    var b: byte := 7; "
        "    var lb: long? := null; "
        "    lb := b; "
        "    Console.WriteLine($\"byte {lb}\"); "
        "    var s: short := -9; "
        "    var ls: long? := null; "
        "    ls := s; "
        "    Console.WriteLine($\"short {ls}\"); "
        "  end Main; "
        "end C;",
        "double 3\nbyte 7\nshort -9\n");
}

// per §5.1 (v0.307), fix #5005 — an INLINE `[N]T?` array element target
// (`coerceToNullableSlot`, LlvmBackend_impl.hpp).
BEZEL_TEST(nullable_widen_reassign_5005, inline_array_element_reassign) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var a: [3]long?; "
        "    var k: int := 4; "
        "    a[0] := k; "
        "    Console.WriteLine($\"inl {a[0]}\"); "
        "  end Main; "
        "end C;",
        "inl 4\n");
}

// per §5.1 (v0.307), fix #5005 — a heap `Array<T?>` indexer-set target
// (LlvmBackend_Assign.cpp's `lowerCollectionIndexerSet`). Pre-fix this was
// an IR VERIFICATION FAILURE ("Invalid InsertValueInst operands!"), not a
// silent wrong value — a stricter oracle than a value assertion, per the
// project's "assert the specific pre-fix failure, not just a value" rule
// for a pre-fix crash/IR-verifier failure. This test asserts the
// post-fix value directly; the compile succeeding at all (no
// `diagnostics.hasErrors()`, `r.ran`) is itself part of what FAIL-WITHOUT
// verifies against the pre-fix IR-verifier abort.
BEZEL_TEST(nullable_widen_reassign_5005, heap_array_indexer_set) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var k: int := 4; "
        "    var a: Array<long?> := new Array<long?>(3, null); "
        "    a[0] := k; "
        "    Console.WriteLine($\"heapArr {a[0]}\"); "
        "  end Main; "
        "end C;",
        "heapArr 4\n");
}

// per §5.1 (v0.307), fix #5005 — a `Dictionary<K, T?>` indexer-set target's
// VALUE side (LlvmBackend_Assign.cpp's `encodeValue` lambda). Same pre-fix
// IR-verifier-failure mode as the Array case above.
BEZEL_TEST(nullable_widen_reassign_5005, dictionary_indexer_set_value) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var k: int := 4; "
        "    var d: Dictionary<string, long?> := new Dictionary<string, long?>(); "
        "    d[\"x\"] := k; "
        "    var v: long? := d[\"x\"]; "
        "    Console.WriteLine($\"dict {v}\"); "
        "  end Main; "
        "end C;",
        "dict 4\n");
}

// per §5.1 (v0.307), fix #5005 — the `??=` null-coalescing-assignment sibling
// shape: same bug pattern in the SAME file's `??=` short-circuit arm (its own
// self-contained `coerceNumericToType(rhs, elemTy, …)` call, distinct from
// the shared `:=` preamble). MEASURED pre-fix: `x ??= k;` (`k: int`,
// `x: long?` starting `null`) printed `0` instead of the widened `4`.
BEZEL_TEST(nullable_widen_reassign_5005, null_coalescing_assign_widen) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var k: int := 4; "
        "    var x: long? := null; "
        "    x ??= k; "
        "    Console.WriteLine($\"coaleq {x ?? -1}\"); "
        "  end Main; "
        "end C;",
        "coaleq 4\n");
}

// per §5.1 / §28.3 (v0.307), fix #5005 — a loop-amplified row: 60 iterations
// each reassigning a fresh `long?` local from a narrower `int` loop variable
// and summing the widened values, pinning the fix under repetition rather
// than a single call.
BEZEL_TEST(nullable_widen_reassign_5005, loop_60_iterations_sum) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var sum: long := 0; "
        "    for var i in 0..<60 do "
        "      var x: long? := null; "
        "      x := i; "
        "      sum := sum + (x ?? -1); "
        "    end; "
        "    Console.WriteLine($\"sum {sum}\"); "
        "  end Main; "
        "end C;",
        "sum 1770\n");
}
