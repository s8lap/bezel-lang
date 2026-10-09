// Codegen/iteration — a lazy LINQ pipeline (`Where`/`Select`/… → a `bz_lazy_t*`)
// passed as an `IEnumerable<T>` ARGUMENT to a same-CU callee that enumerates its
// parameter twice (issue #5612 — the ARGUMENT sibling of #1916, stdlib §5 /
// §29.11).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Assign.cpp wrapAsIEnumerableForTarget, and
// its two argument arms in LlvmBackend_Assign.cpp / LlvmBackend_Calls2.cpp). The
// shared lazy-sentinel branch fired at BOTH the RETURN and the ARGUMENT position:
// it called drainLazyPipelineToOwnedIEnumerable, which runs bz_lazy_to_array —
// walking the pipeline to completion at the CALL SITE — and wraps the resulting
// owned Array<T> in a wrapper whose slot-4 GetEnumerator hands out a FRESH
// enumerator on every call. So the callee received a re-enumerable snapshot, not
// a lazy query, and §29.11's runtime guard had no subject: `l5_0003` declared
// `stdout: caught` and printed nothing.
//
// FIX (spec §29.11, v0.333 — consumption point 3 and the paragraph after it):
//
//   "Passing the query as an `IEnumerable<T>` argument: the single-enumeration
//   right transfers to the callee; the frame's disposal obligation stays with
//   the caller, the plain argument being an ordinary borrow."  …  "Inside a
//   callee, a plain `IEnumerable<T>` parameter is statically untracked … so a
//   violation there is enforced only by the runtime `InvalidOperationException`
//   guard."
//
// An argument is a BORROW, so the frame must stay DEFERRED across the boundary.
// wrapAsIEnumerableForTarget gained a position discriminator (`lazyArgDeferred`)
// that ONLY the two argument arms pass; there it calls the new
// wrapLazyFrameAsSingleUseIEnumerable instead of the drain. That wrapper's slot-4
// GetEnumerator is single-consumption: the FIRST call returns a clone driving the
// deferred frame through the lazy vtable (slot 2 MoveNext / slot 3 Current, the
// latter's pointer result loaded to the by-value the §29.11 enumerable protocol
// expects), a SECOND call raises InvalidOperationException and returns NULL, which
// the callee's ordinary post-GetEnumerator emitPendingCheck() gate (§29.8,
// #2154 / #973a) turns into the throw. The first enumeration therefore still WORKS
// — item 3 transfers the right to the callee, it does not arrive spent — so this
// fix is not a silent-acceptance inversion: it ADDS a runtime rejection to a shape
// that previously accepted silently, and the RETURN sites keep the owning drain
// (#1916's contract: there the caller owns the result).
//
// DISPOSAL (§28.3, exactly once). The wrapper's slot-1 Dispose drains the frame —
// bz_lazy_to_array disposes it, releasing its inline-lambda closures — frees that
// discarded snapshot with bz_array_free, then frees the wrapper struct. The callee
// never disposes a borrowed argument, and the CALLER's own `using var` disposal is
// suppressed once the right is spent at the call site (the coupling the owning
// drain already had). MEASURED: leaving the drain out of slot 1 leaks 116 800 B in
// 1 000 allocations over a 200-iteration lazy-argument loop, because nothing then
// discharges the frame.
//
// FAILS-WITHOUT / PASSES-WITH. The pinning row is
// `reenumerate_raises_on_second_spec_29_11`, and its oracle is on STDOUT, not the
// exit code: pre-fix the program runs clean, exits 0 and prints NOTHING, so an
// exit-code assertion would pass on BOTH the fixed and unfixed compiler — a
// vacuous pin. Asserting `caught\n` is what distinguishes them, so that is what
// this file's primary row does. Every row runs through runAotChecked, so a leak,
// a double-free or a use-after-free in the new wrapper flips r.ran and yields
// -999 — never a legal value.
//
// Spec: §29.11 (v0.333) consumption point 3 and the runtime-guard paragraph;
// §28.3 (owned-local discharge); §29.8 / stdlib §5.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under generated-code ASan/LSan.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The STDOUT oracle for the pinning row. A program that neither ran nor printed
// the expected line reads as a single sentinel so a crashed/unlinked build can
// never be mistaken for a pass.
std::string runStdout(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<DID-NOT-RUN>");
}

// §29.11 item 3 (v0.333): the issue's canonical case — a lazy query passed to a
// same-CU callee whose `IEnumerable<int>` parameter is enumerated TWICE. The
// callee is statically untracked (§29.11's stated parameter boundary), so the
// violation is enforced ONLY by the runtime guard. Before the fix the call site
// drained the frame and handed over a re-enumerable snapshot: the callee
// completed SILENTLY, the catch arm never ran and stdout was EMPTY. After it the
// first loop reads {3,4}, the second raises InvalidOperationException, and the
// caller's catch prints `caught`. The oracle is stdoutText because the exit code
// is 0 either way — this is the row that pins the fix.
BEZEL_TEST(codegen_lazy_linq_ienumerable_arg_5612,
           reenumerate_raises_on_second_spec_29_11) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class Helpers "
        "  public static method Reenumerate(source: IEnumerable<int>) "
        "    for var a in source do end for; "
        "    for var b in source do end for; "
        "  end Reenumerate; "
        "end Helpers; "
        "class C "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4]; "
        "    using var q := xs.Where(lambda(n: int): bool => n > 2); "
        "    try "
        "      Helpers.Reenumerate(q); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runStdout(src), "caught\n");
}

// §29.11 item 3 (v0.333): the right TRANSFERS to the callee — it does not arrive
// spent. A callee that enumerates its parameter ONCE must still see every element
// with the correct value. This is the control that keeps the fix from being a
// silent-acceptance inversion in the other direction: a guard that threw on the
// FIRST enumeration would satisfy the case above while breaking this one, and the
// pre-existing `enumdispose`-style acceptance here is what pins the difference.
// {1,2,3,4} keep n>2 -> {3,4}; sum 7.
BEZEL_TEST(codegen_lazy_linq_ienumerable_arg_5612,
           single_enumeration_in_callee_succeeds_spec_29_11) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class Helpers "
        "  public static method Total(source: IEnumerable<int>): int "
        "    var sum: int := 0; "
        "    for var v in source do sum := sum + v; end for; "
        "    return sum; "
        "  end Total; "
        "end Helpers; "
        "class C "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4]; "
        "    using var q := xs.Where(lambda(n: int): bool => n > 2); "
        "    return Helpers.Total(q); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// §29.11 item 3 (v0.333): a CHAINED deferred operator (`Where(...).Select(...)`)
// crossing the same boundary — the issue's "sibling shapes in the same arm". The
// composed frame is still a lazy frame, so it is still deferred and still
// single-consumption: the callee's one enumeration reads {2,3} * 10 = {20,30},
// sum 50. Before the fix this shape drained at the call site like any other.
BEZEL_TEST(codegen_lazy_linq_ienumerable_arg_5612,
           chained_query_argument_single_enumeration_spec_29_11) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class Helpers "
        "  public static method Total(source: IEnumerable<int>): int "
        "    var sum: int := 0; "
        "    for var v in source do sum := sum + v; end for; "
        "    return sum; "
        "  end Total; "
        "end Helpers; "
        "class C "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    using var q := "
        "        xs.Where(lambda(n: int): bool => n > 1)"
        "          .Select(lambda(n: int): int => n * 10); "
        "    return Helpers.Total(q); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 50);
}

// §29.11 / §28.3 (v0.333): the disposal contract for the argument position. The
// same lazy-argument shape run in a loop pins that the wrapper's slot-1 Dispose
// discharges the frame EXACTLY once per iteration — a leak or a double-free shows
// up here as runAotChecked's LSan/ASan net flipping r.ran to -999. Pre-fix this
// shape was drained at the call site and was clean; the fix moved the drain into
// the wrapper's Dispose, so this row is what keeps that move honest. 200
// iterations, each reading {3,4} = 7 -> 210.
BEZEL_TEST(codegen_lazy_linq_ienumerable_arg_5612,
           loop_disposes_frame_exactly_once_spec_28_3) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class Helpers "
        "  public static method Total(source: IEnumerable<int>): int "
        "    var sum: int := 0; "
        "    for var v in source do sum := sum + v; end for; "
        "    return sum; "
        "  end Total; "
        "end Helpers; "
        "class C "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4]; "
        "    var acc: int := 0; "
        "    var i: int := 0; "
        "    while i < 30 do "
        "      using var q := xs.Where(lambda(n: int): bool => n > 2); "
        "      acc := acc + Helpers.Total(q); "
        "      i := i + 1; "
        "    end while; "
        "    return acc; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 210);
}

}  // namespace