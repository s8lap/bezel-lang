// Disposal-correctness — the v0.209 arm-position transfer marker `owned x`
// (issue #2080; spec §28.3 K3.4/K7.4/A7, §20, §19.2 — v0.209).
//
// §28.3 (v0.209): the marked arm is an OWNED producer for the construct's K7
// meet, with CONSUME-IF-SELECTED semantics — "the binding is consumed on the
// path where its arm is selected and joins per K7.4's existing
// maybe-consumed machinery after the construct (per-path scope-end
// disposal)". §28.3's equalization Pattern 2 gains its one-expression form:
// `var x := (if useFresh then new Stream() else owned cached);`.
//
// Each case pins the disposal accounting natively under the checked oracle
// (runAotChecked — ASan/LSan): the WINNER is disposed exactly once by the
// construct's consumer; the LOSER (the not-selected marked binding) is
// disposed exactly once by the K7.4 per-path scope-end disposal (codegen:
// the marker lowering nulls the slot on the selected path; the scope-end
// dispose is null-guarded). A missed loser is a direct leak; a double
// dispose of the winner is a double-free — both flip r.ran under ASan/LSan.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

const char* kResClass =
    "module M; "
    "class Res "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Res; ";

} // namespace

// §28.3 Pattern 2 (v0.209): the one-expression coalesce — `(if useFresh then
// new Res(1) else owned cached)`. BOTH paths: the winner disposed once by
// the explicit Dispose; the loser (cached, when the fresh arm is selected)
// disposed once by the per-path scope-end disposal; on the marked path
// cached's slot is nulled so scope end skips it.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_pattern2_coalesce_both_paths) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Pick(useFresh: bool): int "
        "    var cached := new Res(9); "
        "    var x := (if useFresh then new Res(1) else owned cached); "
        "    var v := x.Id; "
        "    x.Dispose(); "
        "    return v; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var a := C.Pick(true); "
        "    var b := C.Pick(false); "
        "    Console.WriteLine(\"A\" + a.ToString() + \"B\" + b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Pick(true): D1 (explicit) + D9 (per-path scope end). Pick(false): D9
    // (explicit — cached transferred into x). Exactly-once accounting: one
    // D1, two D9 across the program.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "A1B9"), 1);
}

// §19.2 (v0.209): expression-`case` arm results — two named owned bindings
// coalesced by arm selection; the winner adopted by a `using var`, the
// loser per-path-disposed at scope end. Both selections exercised.
BEZEL_TEST(marked_arm_transfer_2080, spec_19_2_case_arm_results_both_ways) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Sel(n: int): int "
        "    var a := new Res(1); "
        "    var b := new Res(2); "
        "    using var x := (case n of 1 => owned a, _ => owned b); "
        "    return x.Id; "
        "  end Sel; "
        "  public static method Main(): int "
        "    var r1 := C.Sel(1); "
        "    var r2 := C.Sel(5); "
        "    Console.WriteLine(\"R\" + r1.ToString() + r2.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Each Sel call disposes BOTH Res instances exactly once (winner via the
    // using var, loser via per-path scope end): two D1, two D2 total.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R12"), 1);
}

// §19.2 (v0.209): `when` arm results — "semantically identical to `case`
// arms for the meet, admitted for parity".
BEZEL_TEST(marked_arm_transfer_2080, spec_19_2_when_arm_results) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Sel(c: bool): int "
        "    var a := new Res(1); "
        "    using var x := (when c => owned a, _ => new Res(2)); "
        "    return x.Id; "
        "  end Sel; "
        "  public static method Main(): int "
        "    var r1 := C.Sel(true); "
        "    var r2 := C.Sel(false); "
        "    Console.WriteLine(\"R\" + r1.ToString() + r2.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Sel(true): D1 via using var (a adopted); no D2 (fresh arm not run).
    // Sel(false): D2 via using var + D1 per-path. Totals: D1 ×2, D2 ×1.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R12"), 1);
}

// §28.3 K7 (v0.209): the SAME binding marked in both branches — owned ⊓
// owned with one obligation; fully consumed after the construct (MovedFrom),
// disposed exactly once via the adopting consumer on either path.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_same_binding_both_arms) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Both(c: bool): int "
        "    var a := new Res(7); "
        "    using var x := (if c then owned a else owned a); "
        "    return x.Id; "
        "  end Both; "
        "  public static method Main(): int "
        "    var r := C.Both(true) + C.Both(false); "
        "    Console.WriteLine(\"R\" + r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 2);  // one per call
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R14"), 1);
}
