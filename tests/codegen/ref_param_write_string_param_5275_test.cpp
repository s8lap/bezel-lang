// Codegen / refcounting — issue #5275: a by-value `string` parameter passed as
// a `ref` argument to a callee that WRITES the ref parameter aborts with
// `refcount over-release (string)`.
//
// Spec: §22.3 ("Parameter modifiers" / "`ref` and `out` argument locations" —
// "A `ref` argument naming a mutable local or writable field is a true alias:
// callee writes are visible through the caller's location immediately", and
// "`ref` — pass by reference. The caller writes `ref` at the call site"),
// §28.3 (marker economy: "An unmarked read of a class-typed binding never
// consumes", so a by-value `string` parameter is a BORROW of the CALLER's
// reference), §28.9 (runtime-managed types: a `string` is freed at a
// statically-known point, so every reference is released exactly once),
// §27.5 is the closure/capture-cell rule but states no refcount detail; the
// capture-store's own comment (a captured `ref`/`out` param "stays at semaTy =
// nullptr … the envdtor releases nothing") is the citation for the no-retain
// half that keeps a scope-end release off the caller's slot. Spec
// revision v0.313.
//
// THE DEFECT. `W(ref r: string, i)` doing `r := "m" + i.ToString();` routes
// through lowerStringRefParamStore, whose §28.9 balance is load-old /
// retain-or-transfer-new / store-new / RELEASE-OLD. That release-old drops the
// reference the CALLER's slot was holding — and by §28.3 a by-value `string`
// parameter holds the CALLER's reference, not one of its own. The intermediate
// frame therefore frees a reference it never owned. The issue's own repro
// measured `bezel: fatal: refcount over-release (string)`, exit 134.
//
// WHY THE CALLEE'S BODY DECIDES. `ref` carries no "the callee definitely
// assigns it" guarantee — that is `out`'s, and it is why #5253 could count an
// `out` argument as a write unconditionally while leaving `ref` out. A `ref`
// callee that only READS the formal discharges nothing, so a retain for it
// would be a per-call leak with no release to pair it. refFormalWriteVerdict
// (LlvmBackend_Setup.cpp) therefore consults the callee's OWN body: a formal it
// assigns — or passes onward `ref`/`out`, which discharges one level down —
// makes the caller's slot a writer's slot.
//
// Every row below runs under runAotChecked: the full AOT path with generated-code
// AddressSanitizer and LeakSanitizer, so an over-release ABORT, a
// use-after-free, a leak, or a value read out of freed memory all fail the row.
// Each program loops 100× so the refcounts are real and a one-shot row cannot
// pass vacuously, and every string is built at RUNTIME.
//
// USER rows (fail on the pre-fix tree with the over-release abort):
//   1. the issue's own repro, non-nullable `string`;
//   2. the `string?` spelling the issue records as identical;
//   3. a ref-FORWARDING chain (`W1(ref p) { W2(ref p) }`), where the discharge
//      happens one level below the call site that must mint the reference.
// CONTROL rows (pass on BOTH trees — they are what makes the user rows mean
// something):
//   4. a by-value `string` parameter written DIRECTLY (no `ref` argument), i.e.
//      #5253's own shape: unchanged by this fix;
//   5. a by-value `int` parameter passed to a WRITING `ref` callee: the
//      non-`string` formal is outside lowerStringRefParamStore's surface, so
//      nothing here may change;
//   6. a by-value `string` parameter passed to a READ-ONLY `ref` callee: the
//      verdict is NoWrite, so no retain is minted and no scope-end release is
//      armed — LSan pins that (a minted retain with no release would leak one
//      string per call).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr int kIters = 100;

// Total of `len("q" + i)` for i in 0..<100: 10 one-digit (2) + 90 two-digit (3)
// = 20 + 270 = 290. Every user row replaces "q" with "m" one-for-one, so the
// post-write length sum is the same 290 — a wrong length (a value read after
// the write was freed, or before it happened) would move this.
const char* kWant = "290\n";

std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---- USER rows: the over-release ------------------------------------------------

// Issue #5275's verbatim repro. Pre-fix: `bezel: fatal: refcount over-release
// (string) — two releases for one reference`, exit 134.
BEZEL_TEST(codegen_ref_param_write_5275,
           user_byvalue_string_param_to_writing_ref_callee_spec_22_3_28_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Program "
        "  static method W(ref r: string, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end W; "
        "  static method G(p: string, i: int): int "
        "    W(ref p, i); "
        "    return p.Length; "
        "  end G; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + G(\"q\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        std::string(kWant));
}

// The `string?` spelling. The issue records it as the same defect:
// lowerStringRefParamStore balances a nullable target identically (#5253 widened
// it), so the release-old fires just the same.
BEZEL_TEST(codegen_ref_param_write_5275,
           user_byvalue_nullable_string_param_to_writing_ref_callee_spec_22_3_28_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Program "
        "  static method W(ref r: string?, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end W; "
        "  static method G(p: string?, i: int): int "
        "    W(ref p, i); "
        "    return (p ?? \"\").Length; "
        "  end G; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + G(\"q\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        std::string(kWant));
}

// A ref-FORWARDING chain. `W1` never assigns the formal itself — it passes it
// on — so a verdict that only looked for an assignment would stop here, leave
// the caller's slot without a reference, and the write one level down would
// over-release exactly as in row 1. Counting an onward `ref`/`out` argument as
// a discharge trigger is what keeps the chain terminating at its writer.
BEZEL_TEST(codegen_ref_param_write_5275,
           user_ref_forwarding_chain_to_writing_ref_callee_spec_22_3_28_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Program "
        "  static method W2(ref r: string, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end W2; "
        "  static method W1(ref r: string, i: int) "
        "    W2(ref r, i); "
        "  end W1; "
        "  static method G(p: string, i: int): int "
        "    W1(ref p, i); "
        "    return p.Length; "
        "  end G; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + G(\"q\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        std::string(kWant));
}

// ---- CONTROL rows: unchanged on both trees -------------------------------------

// CONTROL 4. A by-value `string` parameter written DIRECTLY — #5253's own shape
// (`p := c.Val`), with no `ref` argument anywhere. Passes on the pre-fix tree
// and must still pass: the fix adds a `ref`-argument arm beside the `out` one,
// it does not touch this path.
BEZEL_TEST(codegen_ref_param_write_5275,
           control_byvalue_string_param_written_directly_spec_28_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Program "
        "  static method Take(x: string, i: int): int "
        "    x := \"m\" + i.ToString(); "
        "    return x.Length; "
        "  end Take; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + Take(\"q\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        std::string(kWant));
}

// CONTROL 5. A by-value `int` parameter passed to a WRITING `ref` callee.
// lowerStringRefParamStore bails on a non-`string` target, so nothing is
// released through the alias and the refFormalWriteVerdict string-type gate
// must answer NoWrite: the value is an immediate, and `2 * (0 + … + 99)` = 9900
// pins the arithmetic end to end.
BEZEL_TEST(codegen_ref_param_write_5275,
           control_byvalue_int_param_to_writing_ref_callee_spec_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Program "
        "  static method WInt(ref r: int, i: int) "
        "    r := i * 2; "
        "  end WInt; "
        "  static method G(p: int, i: int): int "
        "    WInt(ref p, i); "
        "    return p; "
        "  end G; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + G(i, i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        std::string("9900\n"));
}

// CONTROL 6. A by-value `string` parameter passed to a READ-ONLY `ref` callee.
// `Peek` neither assigns the formal nor passes it onward, so the verdict is
// NoWrite: the fix must mint NO retain and arm NO scope-end release here. LSan
// is what makes this row load-bearing — a retain with no release would leak one
// string per call, and the pre-fix tree leaks nothing precisely because it
// mints nothing. Total = Σ(len("ab" + i) + i) = 390 + 4950 = 5340.
BEZEL_TEST(codegen_ref_param_write_5275,
           control_readonly_ref_callee_mints_no_retain_spec_22_3_28_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Program "
        "  static method Peek(ref r: string, i: int): int "
        "    return r.Length + i; "
        "  end Peek; "
        "  static method G(p: string, i: int): int "
        "    return Peek(ref p, i); "
        "  end G; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + G(\"ab\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        std::string("5340\n"));
}
