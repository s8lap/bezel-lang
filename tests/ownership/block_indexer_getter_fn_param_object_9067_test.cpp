// Regression tests for issue #9067 — §28.3 *Set-erasing positions* (v0.359)
// at an indexer's BLOCK getter: the block twin of #9063.
//
// THE DEFECT.  §28.3 *Provenance* clause 3 (v0.359): "A plain function-typed
// parameter of the enclosing body contributes itself: its signature announces
// no set (K2.4), so the value is bounded by the call, §28.14's call-bounded
// tier."  A block getter's set-erasing walk
// (`checkSetErasingPositionsInAccessorBody`) ran on a fresh, unseeded
// `MethodOwnership`, so an indexer's own plain function-typed index parameter
// read as capture-free and `get => do return Sink.KeepS(q); end;` passed it to
// an `Object` parameter unchallenged.  The METHOD twin of the same body, and
// the `=>` accessor forms since #9063, are rejected.  (The setter half of the
// issue already runs the full pass since #9073 and is rejected on the base.)
//
// THE SPEC.  §28.3 *Set-erasing positions* (v0.359): a function value whose
// source set is non-empty may not be passed where the parameter is declared
// `Object` — "Object announces no source set".
//
// ROWS.  The issue's repro and its alias twin (`var g := q;` then `g`), each
// RED on the base tree.  ORACLE.  `runAot`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

void expectObjectReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "function value carries captured borrow(s)"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "may not be passed where the parameter is declared Object"));
}

// `getter` is the indexer's block getter; `Mk` reads the indexer with a
// closure that captures the caller's local `r`, 50 times.
std::string program(const std::string& getter) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "end Res; "
           "public class Sink "
           "  public static method KeepS(o: Object): int return 1; end KeepS; "
           "end Sink; "
           "public class Fl "
           "  public constructor() end constructor; "
           "  public property [q: () -> int]: int "
           "    get => do " +
           getter +
           " end; "
           "  end; "
           "end Fl; "
           "public class Program "
           "  public static method Mk(fl: Fl, r: Res): int "
           "    return fl[lambda() => r.V]; "
           "  end Mk; "
           "  public static method Main(): int "
           "    var fl := new Fl(); "
           "    var t := 0; "
           "    for var i in 0 ..< 50 do "
           "      var r := new Res(); "
           "      t := t + Mk(fl, r); "
           "    end for; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// REJECT — #9067's repro: the block getter passes its own call-bounded index
// parameter to an `Object` parameter.  RED on the base tree.
BEZEL_TEST(block_indexer_getter_fn_param_object_9067,
           block_getter_passes_call_bounded_index_param_to_object_rejected_spec_28_3) {
    expectObjectReject(runAot(program("return Sink.KeepS(q);")));
}

// REJECT — the alias twin: a local bound to the index parameter carries the
// parameter's set (§28.3 *Provenance*), as the METHOD twin's does.  RED on the
// base tree.
BEZEL_TEST(block_indexer_getter_fn_param_object_9067,
           block_getter_passes_local_alias_of_call_bounded_index_param_to_object_rejected_spec_28_3) {
    expectObjectReject(runAot(program("var g := q; return Sink.KeepS(g);")));
}
