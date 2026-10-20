// Codegen — issue #3416 (§21.1 runtime `const` locals, §28.9 runtime-managed
// types, §35.4 choice classification, spec v0.283): a §21.1 runtime `const`
// local bound to a runtime STRING producer never released the string, while
// the `var` spelling of the identical program was ASan+LSan clean. Measured on
// the pre-fix compiler (`bezelc --sanitize=address`, LeakSanitizer on):
//
//   var p := "ab";  const s := p + "cd";        ->  37 B / 2 allocs
//   var n := 41;    const s := n.ToString();    ->  35 B / 2 allocs
//   var p := "ab";  const o: Object := p+"cd";  ->  37 B / 2 allocs
//   const a := Msg.Note(p + "llo");             ->  38 B / 2 allocs
//   var p := "ab" + z;  const s := p;  x500   ->  18500 B / 1000 allocs (!)
//   the first shape inside a 2000-iteration loop -> 74000 B / 4000 allocs
//
// ROOT CAUSE — the FIFTH instance of one pattern. §21.1 gives a runtime
// `const` its own AST node: `LocalConstDecl : Stmt`, a sibling of
// `LocalDecl : Stmt` with differently-named fields (`value` where a `var` has
// `initializer`) and no `isUsing` at all. A codegen walker written as
// `dynamic_cast<const ast::LocalDecl*>(s)` therefore matches a `var` and
// silently does nothing for a `const`. #2802 found it in the collection-
// literal reclaim scan, #3375 in `collectSuspendLocals`, #3402 in four
// closure-reclaim walkers; #3416 is the same shape in
// `Emitter::collectStringReleaseEligible`, which classifies a body's string
// locals into THREE candidate lists — and all three were LocalDecl-only:
//
//   1. PRODUCER (`isPlusOneStringProducer`): a `const` bound to a concat /
//      interpolation / range-slice / `.ToString()` mint never entered
//      `stringReleaseEligible_`, so `armStringLocalRefcount` never registered
//      the scope-end `bz_string_release` that balances the producer's
//      transferred +1. That is the issue's own 37 B / 35 B repros. Note the
//      arm does not consult the DECLARED type, so `const o: Object := p+"cd"`
//      — the second shape filed under #3417 — is a producer candidate too and
//      is closed by this same hunk, which is why it is pinned here.
//
//   2. BORROW (a non-producer string initializer): a `const s := p;` was not
//      admitted, so it was absent from `stringBorrowAll_` — and #2764's
//      relaxation reads exactly that set to decide whether a copy of a string
//      local is a borrow or an ESCAPE. The `const` alias therefore made the
//      SOURCE escape, and the source's own producer +1 stopped being released.
//      The leak is on `p`, not on `s`: a `const` binding was making a
//      neighbouring `var` leak. This is why the issue insisted the widening be
//      measured on both arms. Widening the collector was NOT sufficient here:
//      `StringEscapeWalker`'s own `LocalConstDecl` arm was an UNCONDITIONAL
//      escape, carrying neither of the two borrow-alias relaxations its
//      sibling `LocalDecl` arm has, so it kept tainting the source. #3416 adds
//      the `borrowRetainedAliases` (#2764) one; the `objectStringBorrowAliases`
//      (#2900) one belongs to #3417, which is what admits an Object-headed
//      `const` to that set in the first place.
//
// ⚠ ORACLE NOTE, learned the hard way on this fix: for the BORROW-ALIAS shape a
// SINGLE execution reports CLEAN under LSan on BOTH sides — the one iteration's
// string is still reachable from the live stack at leak-check time — so a
// one-shot test of it passes vacuously. That row is therefore written as a
// 500-iteration loop, which is the honest oracle. The PRODUCER rows do fail
// without the fix at a single execution and are pinned that way as well.
//
//   3. VALUE-LIKE CHOICE (#2115, §35.4): `const a := Msg.Note(p + "llo");`
//      never entered `valueLikeChoiceStrReleaseEligible_`. The consumer needed
//      widening as well here — `armValueLikeChoiceStrRelease` took a
//      `const ast::LocalDecl*` and bailed on its null guard.
//
// The fix does not add a sixth spelling of the decode. The three candidate
// vectors are widened to `const ast::Node*` and every read of the declaration
// goes through `Emitter::localDeclInitView` — the single authority #3402
// hoisted into LlvmBackend_impl.hpp, which maps LocalDecl -> {initializer,
// type, isUsing} and §21.1's LocalConstDecl -> {value, type, false}.
// `valueLikeChoiceLocalMintsStringPayload` and `armValueLikeChoiceStrRelease`
// now take `const ast::Node*` and decode through the same helper. No other
// consumer changed: `lowerStmt`'s LocalConstDecl arm already routes through
// the same `lowerLocalDecl`, and every reader of the eligibility sets
// (`armStringLocalRefcount`, the two `isTrackedStringLocalForRelease` lambdas
// in LlvmBackend_Exprs.cpp / LlvmBackend_Assign.cpp) keys them by NODE POINTER
// (`const void*`), never by node kind.
//
// WHY IT IS NOT A DOUBLE-FREE (Appendix C). The fix adds no §28.3 disposal
// obligation and creates no second owner. §28.9's first entry is `string`, and
// §21.1's *Ownership* paragraph admits exactly those values as a runtime
// `const`'s initializer — "Admitted: value-typed expressions …, `string` and
// other runtime-managed values, `null` …, and borrows"; what a `const` may NOT
// bind is an owned production, and none of the shapes here is one (a rejected
// `const cfg := LoadConfig();` never reaches codegen). What is released is the
// same single reference the `var` path has always released — the producer's
// transferred +1, or the borrow's own bind-time retain — at the same point,
// under the same conservative escape walk. Exactly-once is EASIER to establish
// for a `const` than for a `var`: §21.1 makes a `const` "not an assignable
// location", so the drop-on-overwrite edge cannot be spelled at all and the
// reassignment machinery (`objectStringReassignedLocals_`, the
// `stringReleaseMode` relaxation) is vacuous for it. An ESCAPING `const` is in
// neither eligibility set and still leaks — the safe direction, unchanged
// (pinned by the returned-string test below).
//
// Every test uses runAotChecked (generated-code ASan with LeakSanitizer ON),
// so `r.ran` is itself the leak assertion; the asserted stdout additionally
// proves the release did not disturb the value. Each `const` test is paired
// with its `var` twin, which was clean before and must stay clean — the pair
// is a PARITY assertion, not a bare leak check.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (the only difference being src/codegen/LlvmBackend_Setup.cpp +
// LlvmBackend_impl.hpp reverted):
//
//   program                                    pre-fix            post-fix
//   const s := p + "cd"                        37 B / 2 allocs    clean
//   var   s := p + "cd"        (control)       clean              clean
//   const s := n.ToString()                    35 B / 2 allocs    clean
//   var   s := n.ToString()    (control)       clean              clean
//   const o: Object := p + "cd"                37 B / 2 allocs    clean
//   var   o: Object := p + "cd" (control)      clean              clean
//   const a := Msg.Note(p + "llo")             38 B / 2 allocs    clean
//   var   a := Msg.Note(p + "llo") (control)   clean              clean
//   const alias of a producer, 500 iterations  18500 B / 1000     clean
//   var   alias of a producer, 500 (control)   clean              clean
//   `string?` const alias, 50 iters (guard)    leak, no UAF       leak, no UAF
//   const producer x 2000 in a while loop      74000 B / 4000     clean
//   return a const-bound producer (guard)      clean              clean
//   const n := q.Length + 6    (guard)         clean              clean
// per §21.1, §28.9, §35.4 (issue #3416)

#include <cstdlib>
#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAot with generated-code ASan on and LeakSanitizer OFF — the oracle for the
// SAFE-DIRECTION guard below, whose expected behavior is a bounded residual
// leak. ASan still aborts on a use-after-free or double-free, which is the
// property that guard asserts. Mirrors the `runAotAsanNoLeak` helper in
// choice_value_like_string_payload_2115_test.cpp.
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
// A checked run reports the program's stdout; a leak (or any ASan finding)
// makes runAotChecked report ran=false, which this maps to a distinctive
// sentinel so a failure diff names the oracle rather than an empty string.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── THE ISSUE'S FIRST REPRO: a §21.1 runtime `const` bound to a string
// CONCATENATION producer. `p` is a `var`, so `p + "cd"` is a runtime
// production, not a §17.14 compile-time constant — that is load-bearing: a
// form-constant initializer is classified a COMPILE-TIME const by §21.1 and
// never reaches this path at all. Pre-fix: 37 bytes / 2 allocations leaked.
// per §21.1 (runtime const), §28.9 (`string` is runtime-managed) (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           const_local_bound_concat_producer_released_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"ab\"; "
        "    const s := p + \"cd\"; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "abcd\n");
}

// ── PARITY CONTROL for the test above. The `var` spelling was already clean
// pre-fix and must stay clean; the pair is the assertion.
// per §21.1, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           var_local_bound_concat_producer_parity_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"ab\"; "
        "    var s := p + \"cd\"; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "abcd\n");
}

// ── THE ISSUE'S SECOND REPRO: a LIBRARY-CALL producer (`int.ToString()` mints
// a fresh +1 string). A different `isPlusOneStringProducer` arm from the
// concat above, same LocalDecl-only scan dropping it. Pre-fix: 35 bytes /
// 2 allocations. // per §21.1, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           const_local_bound_tostring_producer_released_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var n := 41; "
        "    const s := n.ToString(); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "41\n");
}

// ── PARITY CONTROL for the `.ToString()` producer.
// per §21.1, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           var_local_bound_tostring_producer_parity_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var n := 41; "
        "    var s := n.ToString(); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "41\n");
}

// ── DECLARED-HEAD axis, and the shape filed as #3417's second repro: the
// producer arm of collectStringReleaseEligible does not consult the DECLARED
// type, so an `Object`-headed `const` over a string producer is a producer
// candidate through the very same scan. It is pinned HERE because this hunk is
// what closes it (collectObjectStringReleaseEligible admits only a BARE
// string IDENTIFIER initializer, never a BinaryExpr). Pre-fix: 37 bytes /
// 2 allocations. // per §21.1, §28.9, §8.6 (issue #3416, #3417)
BEZEL_TEST(const_local_string_release_3416,
           const_object_headed_local_over_string_producer_released_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"ab\"; "
        "    const o: Object := p + \"cd\"; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "abcd\n");
}

// ── PARITY CONTROL for the Object-headed producer.
// per §28.9, §8.6 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           var_object_headed_local_over_string_producer_parity_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"ab\"; "
        "    var o: Object := p + \"cd\"; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "abcd\n");
}

// ── THIRD CANDIDATE ARM (#2115 / §35.4): a VALUE-LIKE choice `const` whose
// construction stores a +1 producer string into the active case's payload
// slot. This arm needed the CONSUMER widened too — `armValueLikeChoiceStrRelease`
// took a `const ast::LocalDecl*` and returned early on its null guard even
// once the collector admitted the node. Pre-fix: 38 bytes / 2 allocations.
// per §21.1, §28.9, §35.4 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           const_value_like_choice_payload_released_spec_21_1_35_4) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"he\"; "
        "    const a := Msg.Note(p + \"llo\"); "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "ok\n");
}

// ── PARITY CONTROL for the value-like choice payload.
// per §28.9, §35.4 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           var_value_like_choice_payload_parity_spec_35_4) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"he\"; "
        "    var a := Msg.Note(p + \"llo\"); "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "ok\n");
}

// ── BORROW ARM — the subtlest row, and the reason the issue asked for both
// arms to be measured. The leaking reference here belongs to the `var` `p`,
// NOT to the `const` `s`. Two independent things had to be fixed for it:
//
//   (a) `collectStringReleaseEligible`'s BORROW candidate list is LocalDecl-
//       only, so the `const` never entered `stringBorrowAll_` and got no
//       bind-time retain; and
//   (b) `StringEscapeWalker`'s own `LocalConstDecl` arm was an UNCONDITIONAL
//       escape — it carried neither of the two borrow-alias relaxations
//       (#2764's `borrowRetainedAliases`, #2900's `objectStringBorrowAliases`)
//       that the sibling `LocalDecl` arm has. So even after (a) the `const`
//       alias still tainted `p`, and `p`'s producer +1 was never released.
//
// ⚠ THE LOOP IS LOAD-BEARING. A SINGLE execution of this shape reports CLEAN
// under LSan — the last (only) iteration's string is still reachable from the
// live stack when the leak check runs — so a one-shot spelling of this test
// passes VACUOUSLY, on both sides of the fix. Measured at 500 iterations:
// 18500 bytes in 1000 allocations pre-fix (one 32 B header + 5 B buffer per
// iteration), clean post-fix, with the `var` twin clean on both sides.
// per §21.1, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           const_borrow_alias_keeps_source_release_armed_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var z := \"cd\"; "
        "      var p := \"ab\" + z; "
        "      const s := p; "
        "      acc := acc + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2000\n");
}

// ── PARITY CONTROL for the borrow alias: the `var` alias was already a
// recognized borrow (#2764), so the source kept its release and the same
// 500-iteration loop was clean before the fix and stays clean.
// per §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           var_borrow_alias_keeps_source_release_armed_parity_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      var z := \"cd\"; "
        "      var p := \"ab\" + z; "
        "      var s := p; "
        "      acc := acc + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2000\n");
}

// ── The `const`-alias relaxation must NOT reach a binding the collector never
// admitted, or the source would be released while an un-retained alias still
// named it (a use-after-free — strictly worse than the leak). A `string?`-
// declared `const` alias is exactly such a binding: the BORROW arm admits only
// a NON-NULLABLE `string`, so it is absent from `stringBorrowAll_`, gets no
// bind-time retain, and must keep tainting `p`. Under ASan a wrong release here
// would surface as a use-after-free at the `s!.Length` read, not as a leak; the
// residual (bounded, one string per iteration) is the safe direction and is why
// this runs with leak detection deliberately OFF.
// per §21.1, §28.9, §28.3 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           nullable_const_alias_still_taints_source_no_uaf_spec_28_3) {
    auto r = runAotAsanNoLeak(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      var z := \"cd\"; "
        "      var p := \"ab\" + z; "
        "      const s: string? := p; "
        "      acc := acc + s!.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// ── UNBOUNDEDNESS axis: the leak is per EXECUTION of the declaration, so a
// loop multiplies it. 2000 iterations leaked 74000 bytes in 4000 allocations
// pre-fix. Also proves the scan RECURSES into a `while` body for the `const`
// node exactly as it does for a `var` one. The accumulated length is asserted
// so the release cannot be "fixed" by never materialising the string.
// per §21.1, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           const_producer_in_loop_body_released_each_iteration_spec_21_1) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var q := \"ab\"; "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 2000 do "
        "      const s := q + \"cd\"; "
        "      acc := acc + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8000\n");
}

// ── SAFETY GUARD (clean before AND after): an ESCAPING `const`-bound producer
// — returned from the method — is in NEITHER eligibility set, so no scope-end
// release is armed and the caller's use is not a use-after-free. Under
// ASan+LSan a spurious release would surface as a heap-use-after-free or a
// double-free abort, not a leak; this pins that the widening did not arm the
// escaping case. // per §21.1, §28.3, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           returned_const_producer_not_released_at_scope_end_spec_28_3) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): string "
        "    var q := \"ab\"; "
        "    const s := q + \"cd\"; "
        "    return s; "
        "  end Make; "
        "  public static method Main(): int "
        "    Console.WriteLine(Make()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "abcd\n");
}

// ── SAFETY GUARD (clean before AND after): a NON-string `const` must not be
// dragged into the string machinery by the widened scan. `q.Length + 6` is an
// `int`; the producer/borrow/choice classification must all decline, leaving
// the local entirely unmanaged. A false positive here would call
// bz_string_release on an integer. // per §21.1, §28.9 (issue #3416)
BEZEL_TEST(const_local_string_release_3416,
           non_string_const_local_unaffected_by_widened_scan_spec_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var q := \"ab\"; "
        "    const n := q.Length + 6; "
        "    Console.WriteLine(n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n");
}
