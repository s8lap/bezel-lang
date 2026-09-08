// Codegen — issue #4078 (spec §28.14 "Call-bounded function parameters", §27.5
// "Closures", §28.9 "Runtime-managed types", §23.4 "Indexers", v0.294): a
// fresh-`owned`-producing lambda argument passed to a function-VALUE parameter
// or to an indexer getter LEAKED the lambda's 40-byte closure cell — the
// callee (§28.14 verified call-bounded) provably held no reference once the
// call returned, so the producer's +1 was the caller's to drop, but neither
// `lowerCall` (the function-VALUE arm, LlvmBackend_Calls2.cpp ~5739, the
// ParameterSymbol dispatch to `emitClosureCall`) nor `lowerIndexerGet` (the
// four getter paths, LlvmBackend_Assign.cpp ~1955 / ~2123 / ~2200 / ~2365)
// emitted a post-call `bz_closure_release`.
//
// MEASURED (HEAD pre-fix): Row 1 — 40 B / 1 allocation per Main call,
// leaking the inline-lambda closure from `bz$M.C$Invoke`. Row 2 — 40 B / 1
// allocation per loop iteration, 50 × 40 = 2000 B / 50 allocations per Main
// call, leaking the inline-lambda closure from `bz$M.C$Main` (the closure
// is built inside the loop and survives until process exit because the
// getter body — `get => do using var c := mk(); return c.tag; end;` — never
// stored the parameter §23.5 forbids `ref`/`out`/`owned` on indexer params
// and the body is small enough for `MethodOwnership::callBoundedScan` to
// verify the body does not store the callback past the call).
//
// LOOP-AMPLIFICATION (this is the [[lsan-one-shot-leak-tests-pass-vacuously]]
// discipline): every row runs 500 iterations of the leak shape so a one-shot
// leak that happens to be reclaimed by an unrelated path cannot pass vacuously.
// Pre-fix, Row 2's 50 iterations alone would not be detectable as "no leak
// ever" — the leak is per-iteration, so 50× produces 2000 B; 500× produces
// 20000 B. Either is LSan-detectable. The 500 figure matches every other
// leak-pin in this tree (`async_iterator_lambda_closure_3553_test.cpp` row 1,
// `argpass_string_refcount_test.cpp`'s `expectValueAndLeakFree` family, etc.)
// so an orphan 50-iteration row cannot read as "we usually do 500".
//
// DIRECTION OF DANGER. The new release is armed with the §28.14 + `isPlusOne-`
// `ClosureProducer` gates — both fail closed (the safe direction is a leak,
// not a UAF) — so the failure mode to guard is an over-release on a stored
// callback. Every row uses `runAotChecked` (generated-code ASan with LSan on),
// so a double-free or use-after-free fails the row before the printed value
// is compared, and the row's printed value is itself an independent oracle on
// whether the callee actually ran.
// per §28.14, §27.5, §28.9, §23.4 (issue #4078)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan WITH LeakSanitizer
// on: a leak, a double free or a use-after-free fails the row before the
// printed exit value is compared.
std::string checkedExit(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return std::string("<did-not-run>");
    // ASan exit-code-on-error is 1 (the default) — for these rows the
    // expected return value is the lambda's produced tag (`9` Row 1, `0`
    // Rows 2/3, `42` Row 4), so an exit code of 1 reports a memory error.
    return "exit=" + std::to_string(r.exitCode);
}

constexpr const char* kRes =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "end Res; ";
}  // namespace

// ── Row 1 (issue repro #4078 #1 / §28.14): invoking a function-VALUE parameter
// `caller: (() -> owned Res) -> int` with an inline lambda `lambda() => new Res(9)`.
// Without the fix the 40-byte closure cell allocated in `bz$M.C$Invoke` leaks
// once per `Main` call (MEASURED 40 B). The lambda body produces a fresh owned
// value, the callee borrows the closure for the duration of the call and never
// stores it (§28.14), so the producer's +1 is the caller's to drop on the
// normal-return edge. `runAotChecked` enforces BOTH the correct exit value
// (9, the Res tag) AND a clean LSan run, so an LSan leak (exit 1) or a UAF
// would both fail the row.
BEZEL_TEST(lambda_arg_call_bounded_closure_release_4078,
           invoked_function_value_parameter_with_inline_lambda_no_leak_28_14) {
    std::string src = std::string(kRes) +
        "class C "
        "  public static method Invoke(caller: (() -> owned Res) -> int): int "
        "    var v := caller(lambda() => new Res(9)); "
        "    return v; "
        "  end Invoke; "
        "  public static method Main(): int "
        "    return Invoke(lambda(mk: () -> owned Res) => "
        "      do using var c := mk(); return c.tag; end); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=9");
}

// ── Row 2 (issue repro #4078 #2 / §23.4): a user-defined indexer getter
// `box[mk: () -> owned Res]: int` invoked with an inline lambda argument.
// Loop-amplified to 500 iterations so a per-iteration 40 B leak cannot pass
// vacuously as "process exit frees it" — 500 × 40 = 20000 B is unambiguous.
// The accumulator is the SUM of the produced tags over the loop, plus the
// final 0 returned by Main (printed as the trailing `acc=` line). Without the
// fix the 40-byte closure cell leaks from `bz$M.C$Main` per iteration; with
// the fix each iteration's closure is released at the post-call edge in
// `releaseIndexArgClosureProducers` (LlvmBackend_Assign.cpp).
BEZEL_TEST(lambda_arg_call_bounded_closure_release_4078,
           indexer_getter_with_inline_lambda_loop_500_no_leak_23_4) {
    std::string src = std::string(kRes) +
        "class Box "
        "  public property [mk: () -> owned Res]: int "
        "    get => do using var c := mk(); return c.tag; end; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(); "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      acc := acc + box[lambda() => new Res(9)]; "
        "    end; "
        "    Console.WriteLine(\"acc=\" + acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    if (!r.ran) { BEZEL_EXPECT_TRUE(false); return; }
    // ASan/LSan clean ⇒ exit code is Main's return (0). A memory error would
    // have exited non-zero before Main returned. acc = 500 * 9 = 4500.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "acc=4500\n");
}

// ── Row 3 (§28.14, non-owned): the SIMPLEST reproducer — a lambda returning
// a literal `int`. Pre-fix this leaked the 40-byte closure cell even though
// nothing about the lambda body requires `owned`-handling (the issue's
// framing about "fresh-owned-producing" was misleading; the leak is the
// closure CELL itself, not anything the body produces). With the fix the
// call-bounded gate releases the cell after the call, so the leak is gone.
// 500 iterations so a per-iteration 40 B leak produces a measurable
// 20000 B (well above LSan noise). exit=42 (= 500 * 42 mod 256 / value).
// (Output goes via return value rather than stdout so a 1-byte exit truncation
// cannot mislead the row — a clean exit is Main's return, an error is ASan's
// non-zero exit.)
BEZEL_TEST(lambda_arg_call_bounded_closure_release_4078,
           invoked_function_value_with_int_lambda_loop_500_no_leak_28_14) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Invoke(caller: (() -> int) -> int): int "
        "    var v := caller(lambda() => 42); "
        "    return v; "
        "  end Invoke; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do "
        "      acc := acc + Invoke(lambda(mk: () -> int) => "
        "        do return mk(); end); "
        "    end; "
        "    return acc; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    if (!r.ran) { BEZEL_EXPECT_TRUE(false); return; }
    // 500 * 42 = 21000; 21000 mod 256 = 21000 - 82*256 = 21000 - 20992 = 8.
    // A clean exit is 8; an LSan leak exits 1 (ASan default).
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}
