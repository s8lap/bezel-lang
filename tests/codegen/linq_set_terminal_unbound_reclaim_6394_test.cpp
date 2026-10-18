// Codegen — the dict-backed `Set<T>` terminal arms of the `isArr` block carry
// no reclaim for an unbound `owned` receiver. §28.3 (Unbound owned
// temporaries), §28.9, stdlib §4.10, v0.338.
//
// THE DEFECT. Inside `Emitter::tryLowerCollectionInstanceCall`'s `isArr` block,
// the `Set<T>` / generic-key-backed `Set` member arms reached NO discharge at
// all for a receiver that is itself an unbound `owned`-producing call consumed
// only in receiver position (`f.MakeSet().Terminal()`). #6352 gave `.Count`
// and `.IsEmpty` the reclaim; its siblings in the same block were left behind.
//
// The spec closes the shape question directly. §28.3: "Such a temporary is
// disposed at the end of the full expression in which it is produced, after
// every use of it within that expression has completed" — and, on why a
// name-keyed list is the wrong remedy: "The `owned` return qualifier is the
// single signal that drives the reclaim; there is no special-cased list of
// method names." So each arm carries the SAME
// `freeUnboundOwnedBorrowingArrayTemp` call on the SAME population, placed
// AFTER the operation that consumes the receiver.
//
// WHAT IS PINNED HERE, AND WHY IT IS THE RECEIVER AND NOT THE ARGUMENT.
// `MakeSet().UnionWith(Other())` measured 86,860 B / 1313 allocations — the
// largest of the six, and two mechanisms, not one. Decomposed at 101
// iterations (`bezelc --sanitize=address`):
//
//   MakeSet().UnionWith(bound Other())   44,036 B  <- the RECEIVER
//   MakeSet().UnionWith(new Set<int>())  78,780 B  <- receiver + ARGUMENT
//   bound MakeSet().UnionWith(bound ...)      0 B  <- neither
//
// The unbound-argument half is an ARGUMENT-POSITION temporary (`new Set<int>()`
// in a plain-argument slot), disposed by a different mechanism on a different
// site, and is deliberately NOT claimed here. The `.UnionWith` row below
// therefore passes a BOUND argument, which isolates the receiver — and the
// 35,552 B the unbound-argument twin still measures after this fix is recorded
// as a residual rather than silently absorbed.
//
// A leak pin must prove its detector ran. `runAotChecked` defaults to
// `ASAN_OPTIONS=detect_leaks=0`, so a plain row is VACUOUS for a leak: green on
// a broken tree while showing a clean sanitizer trio. Every row below arms the
// leak oracle with `BEZEL_AOT_DETECT_LEAKS=strict` (save/restored, so a gate
// running the knob does not leak into a sibling row) and asserts
// `!checkedToolchainSkipped(r)` BEFORE any guard that could absorb the skip.
// No `LSAN_OPTIONS` is set: these are found at LSan defaults.
//
// PRE-FIX (101 iterations, `bezelc --sanitize=address` on 04d402b3):
//
//   f.MakeSet().Contains(2)      43,228 B / 606 allocs
//   f.MakeSet().Add(9)           43,632 B /  707
//   f.MakeSet().TryAdd(9)         (same shape as Add)
//   f.MakeSet().Remove(2)        42,824 B /  505
//   f.MakeSet().Clear()          41,600 B /  300
//   f.MakeSet().UnionWith(Other()) 86,860 B / 1313   (receiver + argument)
//
// and 0 for the byte-identical BOUND twin of each — the asymmetry that
// identifies the defect: a bound receiver is discharged by its own owner, and
// these arms reached no discharge at all.
//
// The BOUND rows are not decorative. The fix adds a reclaim where there was
// none, so the failure direction that matters is a DOUBLE free, and only a
// bound-receiver row can see it: those receivers are discharged by their own
// `using var` / auto-drop, and if the gate were wrong they would be freed
// twice.

#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row — see the file header.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// The harness SKIPS (loudly, counted) when clang is absent; a row that
// asserts through that would be asserting on an unarmed oracle. A sanitizer
// finding is NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// The producer module, shared by every row. `MakeSet` is the unbound
// `owned Set<int>` producer whose table the reclaim must reclaim;
// `OtherSet` is a second one, bound by the caller at the algebra row so that
// row measures the RECEIVER and nothing else.
std::string producerModule() {
    return
        "module M; "
        "class P public "
        "  static method MakeSet(): owned Set<int> "
        "    var s: Set<int> := new Set<int>(); "
        "    s.Add(1); s.Add(2); s.Add(3); "
        "    return s; "
        "  end MakeSet; "
        "  static method OtherSet(): owned Set<int> "
        "    var t: Set<int> := new Set<int>(); "
        "    t.Add(4); t.Add(5); "
        "    return t; "
        "  end OtherSet; "
        "end P; ";
}

// 101 iterations — one-shot leak repros are a vacuity trap: a single
// evaluation's table is small enough to hide under LSan noise, and the whole
// point of the defect is that it repeats once per evaluation. The issue's own
// measurements are all at 101 and the byte counts quoted in the header are the
// numbers a 101-iteration loop reproduces.
JitResult runArm(const std::string& armBody) {
    return runAotChecked(
        producerModule() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      " + armBody + " "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
}

// One row: arm the leak oracle, assert the skip is NOT what made it green,
// assert the program actually ran and exited clean, and assert the arm's own
// observable value so a reclaim placed so early that it frees the table before
// the read would fail here too (and not merely "stop leaking").
void expectNoLeak(const char* armBody, int expectedAcc) {
    LeakEnv scope("strict");
    JitResult r = runArm(armBody);
    // #3733: a silent `return` here would report a clean PASS on a box whose
    // ASan link is unavailable — green while asserting nothing about memory
    // safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    if (expectedAcc >= 0) {
        BEZEL_EXPECT_TRUE(r.stdoutText.find(std::to_string(expectedAcc)) !=
                          std::string::npos);
    }
}

}  // namespace

// ===========================================================================
// THE SIX ARMS, unbound `owned Set<int>` receiver consumed only in receiver
// position. 101 iterations each; every one measured 41-44 KB on the parent
// (table + buckets + key copies + insertion-order spine, per evaluation).
// `acc` counts a real observation of the arm's RESULT, so the row asserts the
// arm ran and returned the right thing as well as not leaking.
// ===========================================================================
BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           contains_of_an_unbound_owned_set_is_reclaimed_spec_28_3) {
    expectNoLeak("if P.MakeSet().Contains(2) then n := n + 1; end if;", 101);
}

BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           add_on_an_unbound_owned_set_is_reclaimed_spec_28_3) {
    // `.Add` returns unit, so the row's only observable is that the arm ran;
    // the reclaim's placement (after the insert, not before) is what the leak
    // oracle and the Set<string> row below jointly pin.
    expectNoLeak("P.MakeSet().Add(9);", -1);
}

BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           try_add_on_an_unbound_owned_set_is_reclaimed_spec_28_3) {
    expectNoLeak("if P.MakeSet().TryAdd(9) then n := n + 1; end if;", 101);
}

BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           remove_on_an_unbound_owned_set_is_reclaimed_spec_28_3) {
    expectNoLeak("if P.MakeSet().Remove(2) then n := n + 1; end if;", 101);
}

BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           clear_on_an_unbound_owned_set_is_reclaimed_spec_28_3) {
    // `.Clear()` returns unit — its §28.3 obligation cannot sit at a return
    // site at all, which is exactly why this arm is worth pinning separately.
    expectNoLeak("P.MakeSet().Clear();", -1);
}

BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           union_with_on_an_unbound_owned_set_is_reclaimed_spec_28_3) {
    // BOUND argument on purpose — see the header. An unbound `OtherSet()` here
    // would also leak its own table through the ARGUMENT-position path, a
    // different mechanism this commit does not claim, and would keep the row
    // red after the receiver is fixed.
    expectNoLeak(
        "var o := P.OtherSet(); P.MakeSet().UnionWith(o);", -1);
}

// ===========================================================================
// THE STRING-ELEMENT HALF. A `Set<string>` co-owns a +1 per element
// (§28.11's shared-reference row), so the reclaim's `bz_set_string_keys_release`
// + `bz_dict_free` pair must run, and must not double-discharge what
// `.Add`'s own retain or `.Remove`'s / `.Clear`'s own releasing variant already
// resolved. This is the axis a `Set<int>` row cannot see at all.
// ===========================================================================
BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           string_element_set_arms_discharge_each_reference_once_spec_28_11) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        "module M; "
        "class P public "
        "  static method MakeStrSet(): owned Set<string> "
        "    var s: Set<string> := new Set<string>(); "
        "    s.Add(\"aa\"); s.Add(\"bb\"); "
        "    return s; "
        "  end MakeStrSet; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      if P.MakeStrSet().Contains(\"aa\") then n := n + 1; end if; "
        "      var t := P.MakeStrSet(); "
        "      t.Add(\"cc\"); t.Remove(\"aa\"); t.Clear(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// THE BOUND CONTROL — the direction a reclaim ADDED where none existed can
// break. Each of these receivers is discharged by its own owner (a `using var`
// / auto-drop at scope end, §28.3). If the reclaim's gate ever matched one of
// them, the table would be freed twice and ASan would report the double free
// or the use-after-free on the owner's own disposal. These rows are the
// regression test for that, not a restatement of the unbound rows.
// ===========================================================================
BEZEL_TEST(linq_set_terminal_unbound_reclaim_6394,
           bound_set_receivers_are_not_freed_by_the_member_arms_spec_28_3) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        producerModule() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      var a := P.MakeSet(); "
        "      if a.Contains(2) then n := n + 1; end if; "
        "      a.Add(9); "
        "      var b := P.MakeSet(); "
        "      if b.Remove(2) then n := n + 1; end if; "
        "      var c := P.MakeSet(); "
        "      c.Clear(); "
        "      var d := P.MakeSet(); "
        "      var o := P.OtherSet(); "
        "      d.UnionWith(o); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}