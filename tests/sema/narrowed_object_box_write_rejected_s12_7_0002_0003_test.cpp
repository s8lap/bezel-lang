// Spec-derived regression test for rule-corpus `s12_7_0002` / `s12_7_0003`
// (spec §12.7 / §22.3, v0.307).
//
// THE GOVERNING RULE. Spec §12.7 "Type tests: `is`, `is not`, `as`" (v0.307):
// "A write whose store would land in the box is a compile error, in each of
// the three positions that can produce one: (i) an assignment, compound
// assignment, or `++`/`--` whose target is an lvalue chain (grammar A.6
// `LValue`) extending the binding with a `.` or `[...]` hop — `o[0] := 9`,
// `o.X := 5`, `o[0] += 1`, `o.X++`; (ii) such a chain passed as a `ref` or
// `out` argument, which §22.3's assignable-storage-location categories admit
// (`ref o[0]`, `out o.X`) ... What is *not* rejected is the bare-binding
// compound and `++`/`--` form: `o += 1` and `o++` ... that store targets the
// *declared* `Object`/interface location."
//
// ROOT CAUSE. No sema check consulted `narrowings_` (the §12.7 flow-narrowing
// map) at an lvalue-chain WRITE site at all — the map was read only when
// TYPING an ordinary read of the narrowed identifier. `o[0] := 9` (position
// i) and `Swap(ref o[0], ref o[1])` (position ii) both compiled clean,
// silently writing into the box's shared payload instead of being rejected.
//
// MEASURED on `bf7a7515` (this lane's base, `r19-c20`) with `build/bezelc`:
// both rule-corpus cases (verbatim below) compiled with rc=0.
//
// THE FIX. `ExprTC::narrowedToBoxedValue` (is `sym` currently §12.7-narrowed
// to a VALUE type at a box-holding `Object`/interface declared location?)
// and `ExprTC::checkNarrowedBoxWrite` (walks the lvalue chain exactly as
// `checkLValueChainNotCallBased` does; rejects when at least one `.`/`[...]`
// hop extends such a binding) — src/sema/ExprTypecheck.cpp. Wired into
// `checkReadonlyAssignment` (position i, `:=` and every compound form),
// the `IncrementStmt` arm (position i, `++`/`--`), and
// `checkRefOutOperandCategory` (position ii, `ref`/`out` arguments).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

}  // namespace

// rule-corpus `s12_7_0002`, verbatim: position (i), an indexed assignment
// through a narrowed Object.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           position_i_indexed_assignment_rejected) {
    const char* src =
        "module Conf; "
        "public class Program "
        "  public static method Test() "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      o[0] := 9; "
        "    end if; "
        "  end Test; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "box"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""12.7"));
}

// Position (i) companion: a MEMBER-hop chain-write (`o.X := 5` shape) on a
// narrowed value-builtin location must be rejected the same way — the rule
// names both `.` and `[...]` hops, not just indexing.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           position_i_member_hop_compound_assignment_rejected) {
    const char* src =
        "module Conf; "
        "public record Pt(X: int, Y: int); "
        "public class Program "
        "  public static method Test() "
        "    var p: Pt := Pt(1, 2); "
        "    var o: Object := p; "
        "    if o is Pt then "
        "      o.X += 1; "
        "    end if; "
        "  end Test; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "box"));
}

// Position (i) companion: the `++`/`--` spelling.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           position_i_increment_rejected) {
    const char* src =
        "module Conf; "
        "public class Program "
        "  public static method Test() "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      o[0]++; "
        "    end if; "
        "  end Test; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "box"));
}

// rule-corpus `s12_7_0003`, verbatim: position (ii), a `ref` argument chain
// through a narrowed Object.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           position_ii_ref_argument_rejected) {
    const char* src =
        "module Conf; "
        "public class Program "
        "  public static method Swap(ref x: int, ref y: int) "
        "    var tmp := x; "
        "    x := y; "
        "    y := tmp; "
        "  end Swap; "
        "  public static method Test() "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      Swap(ref o[0], ref o[1]); "
        "    end if; "
        "  end Test; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "box"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""12.7"));
}

// ACCEPT control: READS through the narrowed box (the rule's other half —
// "Reads through the narrowed binding read the box's payload") must keep
// working. Guards against an over-broad fix that rejects every chain hop on
// a narrowed identifier, not just writes.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           narrowed_box_read_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(4,
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      return o[0]; "
        "    end if; "
        "    return -1; "
        "  end Main; "
        "end Program;"));
}

// ACCEPT control: the spec's own copy-out/mutate/re-box remedy — a fresh
// local copy, mutated, then stored back through the DECLARED location via a
// bare (hop-free) reassignment — must stay accepted; only the CHAIN-ROOTED
// forms are rejected.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           copy_out_mutate_rebox_remedy_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(9,
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      b[0] := 9; "
        "      o := b; "
        "    end if; "
        "    if o is [3]int then "
        "      return o[0]; "
        "    end if; "
        "    return -1; "
        "  end Main; "
        "end Program;"));
}

// ACCEPT control: an UNNARROWED plain-var `ref` chain (no §12.7 narrowing in
// play at all) must be entirely unaffected by the new position-(ii) check.
BEZEL_TEST(sema_narrowed_object_box_write_rejected_s12_7_0002_0003,
           unnarrowed_ref_index_chain_still_accepted) {
    BEZEL_EXPECT_TRUE(runs(6,
        "module Conf; "
        "public class Program "
        "  public static method Bump(ref x: int) "
        "    x := x + 1; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    Bump(ref a[1]); "
        "    return a[1]; "
        "  end Main; "
        "end Program;"));
}
