// fix #7667 (§23.4 Indexers / §26.5 Resolution, v0.355): the VALUE side of
// the NON-generic-extension `this[i]` case.
//
// The no-announcement side is pinned by the lit row
// tests/lit/generic_extension_this_indexer_nongeneric_no_dropped_note_7667.bzl
// (runAot fills `diagnostics` only on a reject, so a gtest cannot see a
// Warning on a program that compiles), and the announcement's survival for the
// one shape where the receiver genuinely cannot be recovered is pinned by
// tests/lit/generic_extension_this_indexer_nongeneric_dropped_note_announced_7667.bzl.
//
// THE DEFECT. `indexAccessReceiverDecl` (src/codegen/LlvmBackend_Assign.cpp)
// announces "internal: this `this[i]` indexer access was DROPPED" whenever the
// receiver TypeSymbol cannot be recovered. A built-in collection has NO
// TypeSymbol — its TypeRef's `declaration` is null — so for an extension on
// one the recovery cannot succeed BY CONSTRUCTION, and the access is lowered
// by the COLLECTION indexer path instead (the receiver's sema type is the
// extension's target type, which sema publishes for `this` inside a builtin
// extension body — #456). #4160 covered the UNSPECIALIZED template body of a
// GENERIC extension and #7561 its SPECIALIZATION; the NON-GENERIC block
// (`extension Array<int>`, `extension Array<string>`, …) matched neither: its
// target type carries a CONCRETE element type, so #4160's "a generic argument
// no active substitution binds" gate cannot fire on it.
//
// THE VALUES WERE CORRECT ALL ALONG — the announcement's own text ("the read
// degrades to a zero/null value and a write is silently discarded") is the
// false part. That is what these rows pin: the write `this[0] := this[0] + 10`
// really stores 11, the compound `this[1] += 5` really stores 7, and the
// string swap really exchanges the two elements. A fix that silenced the
// announcement by DROPPING the access would print 1 / 2 here instead.
//
// RunAot (the strict AOT oracle: a fork-isolated `Driver::compileFile` + a
// clang link + a native exec), not the deprecated in-process JIT harness.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string out(const std::string& p) {
    auto r = runAot(p);
    return r.ran ? r.stdoutText : std::string("<did not run>");
}
}  // namespace

// #7667 value side (the no-announcement side is pinned by the lit row
// generic_extension_this_indexer_nongeneric_no_dropped_note_7667.bzl): the
// NON-generic `extension Array<int>` write and compound write through `this[i]`
// store the right values, and the `Array<string>` twin swaps its two elements.
BEZEL_TEST(codegen_generic_extension_this_indexer_nongeneric_7667,
           nongeneric_extension_this_indexer_values_7667) {
    BEZEL_EXPECT_EQ(out(R"(module M;
extension Array<int>
  public method Bump()
    this[0] := this[0] + 10;
    this[1] += 5;
  end Bump;
end;
extension Array<string>
  public method Swap01()
    var t: string := this[0];
    this[0] := this[1];
    this[1] := t;
  end Swap01;
end;
public static class Program
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var ys: Array<string> := ["a", "b"];
    xs.Bump();
    ys.Swap01();
    Console.WriteLine(xs[0]);
    Console.WriteLine(xs[1]);
    Console.WriteLine(ys[0]);
    Console.WriteLine(ys[1]);
    return 0;
  end Main;
end;
)"), std::string("11\n7\nb\na\n"));
}
