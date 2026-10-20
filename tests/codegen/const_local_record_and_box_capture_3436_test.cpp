// Codegen — issue #3436 (§21.1 runtime `const` locals, §9.7 records and
// disposal, §27.5 closures and capture cells, §28.9 runtime-managed types,
// spec v0.283): two more codegen walkers whose candidate scan was spelled
// `dynamic_cast<const ast::LocalDecl*>(s)` silently skipped every §21.1 runtime
// `const`, so the reclaim each of them arms was never armed for one. Measured
// on the pre-fix compiler (`bezelc --sanitize=address`, LeakSanitizer on):
//
//   const p := C.Make(i);        x200  (record call result)  7290 B / 400
//   const p := new Rec(...);     x200  (record construction) 7290 B / 400
//   const o: Object := i; captured by a lambda   x200        4800 B / 200
//
// Every `var` twin of those programs is ASan+LSan clean, before and after. (The
// issue reports the third row as 4776 B / 199 on its own build; both readings
// are 24 bytes per execution of the declaration, differing only in whether LSan
// still saw the final iteration's box as stack-reachable at exit. The full
// measured table is at the bottom of this header.)
//
// ROOT CAUSE — the sixth and seventh instances of the family recorded as
// #2802 / #3375 / #3402 / #3416 / #3417 (structurally, #3419). §21.1 gives a
// runtime `const` its own AST node, `ast::LocalConstDecl`, a sibling of
// `ast::LocalDecl` with differently-named fields (`value` where a `var` has
// `initializer`, and no `isUsing` at all), so a walker that scans for
// `ast::LocalDecl` matches a `var` and does nothing for a `const`. Two sites:
//
//   1. `Emitter::collectRecordCallResultReleaseEligible` — the candidate scan
//      never admitted a `const`, so `const p := Mk();` never entered
//      `recordCallResultReleaseEligible_` and `lowerLocalDecl` never registered
//      the scope-end `recordStringFields` Disposal. Each callee-minted +1 on
//      the record's `string` fields was dropped. Both of that collector's arms
//      were affected: the #1747 record-returning-CALL arm and the #2866
//      `new Rec(...)` fresh-construction arm.
//
//   2. `Emitter::collectBoxCaptureLocals` — same scan shape, feeding the §27.5
//      capture-cell machinery (`boxCaptureLocals_`, the envdtor's
//      `bz_box_release`, and `singleCaptureLocals_`, the envdtor's cell free).
//      This is NOT the `boxReleaseEligible_` path #3417 fixed: a lambda-
//      captured box local is an ESCAPE under `BoxEscapeWalker` for BOTH
//      spellings, so neither enters that set. What frees the box for a `var` is
//      exactly the machinery this collector feeds, and its scan never saw the
//      `const` node — so the producer's single +1 was dropped on the floor.
//
// No new spelling of the LocalDecl / LocalConstDecl split is introduced by the
// fix. Both candidate containers hold `const ast::Node*` and both scans read
// the declaration through `Emitter::localDeclInitView` — the single authority
// #3402 hoisted into LlvmBackend_impl.hpp for exactly this — with (2)
// additionally going through `isPlusOneBoxProducer`, which #3417 already made
// `ast::Node*`-taking on top of that same authority. No consumer changed: every
// one of these sets is keyed by NODE POINTER (`const void*`), never by node
// kind, and `lowerStmt`'s `LocalConstDecl` arm already routes through the same
// `lowerLocalDecl`. `sema::LocalSymbol::decl` likewise already points at
// whichever node declared the local, so `collectBoxCaptureLocals`' capture
// tally needed no change either.
//
// WHY IT IS NOT A DOUBLE-FREE (Appendix C). §21.1's *Ownership* paragraph
// admits, as a runtime `const`'s initializer, "value-typed expressions (records
// included — a borrow-bearing record value carries [§9.7]'s ordinary source
// obligations), `string` and other runtime-managed values, `null` ..., and
// **borrows**", while rejecting an owned production ([§28.3]'s producers) —
// so the binding takes no disposal obligation and this
// adds none. What is reclaimed is codegen's own: the same single reference the
// `var` path has always released, at the same point, under the same
// conservative `StringEscapeWalker` (1) and the same #802 rc-retain model (2 —
// the FIRST capture transfers the producer's +1, every ADDITIONAL capture
// retains, and every env's envdtor releases exactly one). Exactly-once is
// EASIER to establish for a `const` than for a `var`: §21.1 makes it "not an
// assignable location", so the drop-on-overwrite edge does not exist for it,
// it can never reach `assignedCaptureVars_`, and `mutatedSharedCaptureCell`'s
// write-after-capture desynchronisation cannot arise at all.
//
// ⚠ ORACLE NOTE. Every leak row here is written as a 200-iteration loop, and
// that is load-bearing: a ONE-SHOT LSan row can report clean on BOTH sides
// because the single iteration's allocation is still reachable from the live
// stack at leak-check time. The loop also proves the widened scans recurse into
// a `while` body for a `const` node.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (the only difference being this fix's `src/` hunks reverted):
//
//   program                                            pre-fix        post-fix
//   const p := C.Make(i)          x200  (record call)  7290 B / 400   clean
//   var   p := C.Make(i)          x200  (control)      clean          clean
//   const p := new Rec(...)       x200  (record new)   7290 B / 400   clean
//   var   p := new Rec(...)       x200  (control)      clean          clean
//   const p := C.Make(i); keep := p     (escape guard) 7253 B / 398   7253 B / 398
//   var   p := C.Make(i); keep := p     (control)      7253 B / 398   7253 B / 398
//   const o: Object := i, 1 lambda x200 (box capture)  4800 B / 200   clean
//   var   o: Object := i, 1 lambda x200 (control)      clean          clean
//   const o: Object := i, 2 lambdas x200 (#802)        4776 B / 199   clean
//   var   o: Object := i, 2 lambdas x200 (control)     clean          clean
//   returned closure over a const box   x200 (#694)    4800 B / 200   clean
//   returned closure over a var   box   x200 (control) clean          clean
//   const o: Object := <class instance> (guard)        clean          clean
//   var   o: Object := <class instance> (control)      clean          clean
//
// (The 4800/200 vs 4776/199 spread across the two box rows is LSan reachability
// of the final iteration's box at exit, not a difference in what leaks: 24 bytes
// per execution of the declaration either way. It is exactly why these rows are
// loops and not single executions.)
//
// per §21.1, §9.7, §27.5, §28.9 (issue #3436)

#include <cstdlib>
#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// runAot with generated-code ASan on and LeakSanitizer OFF — the oracle for the
// SAFE-DIRECTION guard whose expected behavior is a bounded residual leak that
// is byte-identical to the `var` twin's. ASan still aborts on a use-after-free
// or a double-free, which is exactly what that guard asserts. Mirrors the
// helper in const_local_box_release_3417_test.cpp.
//
// #6608. The SKIP branch also consults the harness's OWN test-only knob,
// `bezel::test::forceToolchainMissing()` (the reader of
// BEZEL_AOT_FORCE_NO_TOOLCHAIN that `runAotCheckedImpl` itself consults).
// This wrapper delegates to `runAot`, which never reads that knob, so before
// this the only way onto the skip path was a box genuinely lacking clang — and
// the CONTROL ARM, the one experiment that proves a converted oracle CAN fail,
// had no way to reach this row. Only the `clang --version` PROBE stays
// memoised (it shells out, and the harness memoises it for the same reason);
// the env-var read deliberately does not, so a per-test setenv can toggle it.
bezel::test::JitResult runAotAsanNoLeak(std::string_view src) {
    static const bool toolchainOk =
        std::system("clang --version > /dev/null 2>&1") == 0;
    if (bezel::test::forceToolchainMissing() || !toolchainOk)
        return runAotChecked(src);  // SKIP diagnostic, counted + asserted on below
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    return runAot(src, opts);
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// (1) collectRecordCallResultReleaseEligible — §9.7 record string fields
// ═══════════════════════════════════════════════════════════════════════════

// ── THE ISSUE'S FIRST REPRO, verbatim: a §21.1 runtime `const` bound from a
// record-returning CALL. §21.1 admits a value-typed expression — records
// included — as a `const`'s initializer, so this is spellable; pre-fix the
// callee-minted +1 on `Rec.Name` was never released (7290 B / 400 allocations
// over 200 iterations). // per §21.1, §9.7, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           const_record_call_result_string_fields_released_spec_21_1_9_7) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "record Rec(Name: string); "
        "class C "
        "  public static method Make(n: int): Rec "
        "    return new Rec(\"r\" + n.ToString()); "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      const p := C.Make(i); "
        "      acc := acc + p.Name.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "690\n");
}

// ── PARITY CONTROL: the `var` spelling of the identical program was clean
// pre-fix and must stay clean. // per §9.7, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           var_record_call_result_string_fields_released_parity_spec_9_7) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "record Rec(Name: string); "
        "class C "
        "  public static method Make(n: int): Rec "
        "    return new Rec(\"r\" + n.ToString()); "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var p := C.Make(i); "
        "      acc := acc + p.Name.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "690\n");
}

// ── THE COLLECTOR'S OTHER ARM (#2866, §28.3's unbound owned temporary): the
// same walker also admits a `new Rec(...)` fresh-construction initializer, and
// that arm read `LocalDecl::initializer` too — so a `const` bound directly to a
// record construction leaked the +1 string producer argument identically.
// per §21.1, §9.7, §28.3, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           const_record_construction_string_fields_released_spec_21_1_28_3) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "record Rec(Name: string); "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      const p := new Rec(\"r\" + i.ToString()); "
        "      acc := acc + p.Name.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "690\n");
}

// ── PARITY CONTROL for the construction arm.
// per §9.7, §28.3, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           var_record_construction_string_fields_released_parity_spec_28_3) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "record Rec(Name: string); "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var p := new Rec(\"r\" + i.ToString()); "
        "      acc := acc + p.Name.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "690\n");
}

// ── SAFETY GUARD (unchanged before AND after, and byte-identical to the `var`
// twin): a `const`-bound record COPIED into a longer-lived local ESCAPES, so
// the shared StringEscapeWalker must keep it OUT of
// `recordCallResultReleaseEligible_`. If the widened scan had admitted an
// escaping `const`, the scope-end release would free a string the surviving
// `keep` still points at, and the post-loop `keep.Name` read would be a
// use-after-free — which is why this runs with leak detection deliberately off:
// the residual 7253 B / 398 allocs is the pre-existing, safe direction and is
// what the `var` spelling leaks too. // per §21.1, §9.7, §28.3 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           escaping_const_record_not_released_no_uaf_spec_21_1_28_3) {
    auto r = runAotAsanNoLeak(
        "module M; import System; "
        "record Rec(Name: string); "
        "class C "
        "  public static method Make(n: int): Rec "
        "    return new Rec(\"r\" + n.ToString()); "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    var keep := new Rec(\"seed\"); "
        "    while i < 200 do "
        "      const p := C.Make(i); "
        "      keep := p; "
        "      acc := acc + keep.Name.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(keep.Name); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_EQ(r.stdoutText, "r199\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// (2) collectBoxCaptureLocals — §27.5 capture cells over a §8.6 box
// ═══════════════════════════════════════════════════════════════════════════

// ── THE ISSUE'S SECOND REPRO, verbatim: an `Object`-headed §21.1 `const` over
// an `int`, CAPTURED by a lambda. The capture is an escape for both spellings,
// so `boxReleaseEligible_` (the set #3417 fixed) deliberately excludes it; what
// frees the box for a `var` is the envdtor's `bz_box_release`, armed from
// `boxCaptureLocals_`, whose scan never saw the `const` node. 4776 B / 199
// allocations over 200 iterations. // per §21.1, §27.5, §8.6, §28.9 (#3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           const_object_box_captured_by_lambda_released_spec_21_1_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      const o: Object := i; "
        "      var f := lambda() => o is int; "
        "      if f() then acc := acc + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\n");
}

// ── PARITY CONTROL: the `var` spelling was clean pre-fix and must stay clean.
// per §27.5, §8.6, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           var_object_box_captured_by_lambda_parity_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var o: Object := i; "
        "      var f := lambda() => o is int; "
        "      if f() then acc := acc + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\n");
}

// ── MULTI-ENV axis (#802): two lambdas capture the same `const` box. The
// rc-retain model must balance — the first capture transfers the producer's +1,
// the second `bz_box_retain`s an independent one, and both envdtors release
// exactly one — so this is the shape that would surface an OVER-release as an
// ASan double-free rather than a leak. // per §21.1, §27.5, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           const_object_box_captured_by_two_lambda_envs_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      const o: Object := i; "
        "      var f := lambda() => o is int; "
        "      var g := lambda() => o is string; "
        "      if f() then acc := acc + 1; end if; "
        "      if g() then acc := acc + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\n");
}

// ── CELL-FREE axis (#694, `singleCaptureLocals_` — the collector's OTHER
// output): a closure that DEFINITELY escapes by being returned frees the
// capture cell it solely holds in its envdtor. The caller INVOKES it after the
// defining scope is gone, so a premature cell free or box release would be an
// ASan use-after-free here rather than a leak.
// per §21.1, §27.5, §28.9 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           returned_closure_over_const_object_box_frees_cell_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Mk(n: int): () -> bool "
        "    const o: Object := n; "
        "    return lambda() => o is int; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f := C.Mk(i); "
        "      if f() then acc := acc + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\n");
}

// ── SAFETY GUARD (clean before AND after): a CLASS instance bound to an
// `Object`-headed `const` and captured is NOT a box, so `isPlusOneBoxProducer`
// must keep answering false for it through the widened scan. Were it admitted,
// the envdtor's `bz_box_release` would run on a class pointer — under ASan a
// wild free, not a leak. // per §21.1, §8.6, §27.5 (issue #3436)
BEZEL_TEST(const_local_record_and_box_capture_3436,
           const_object_over_class_instance_captured_not_box_released_spec_8_6) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class Holder "
        "  public readonly property V: int; "
        "  public constructor(v: int)  V := v;  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var h := new Holder(i); "
        "      const o: Object := h; "
        "      var f := lambda() => o is Holder; "
        "      if f() then acc := acc + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\n");
}
