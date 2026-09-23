// K8.4 at the §35.5 owning-match boundary — the ASSIGNMENT-SOURCED channel: a
// store, return or capture routed through a same-arm local that reaches the
// transferred payload by ASSIGNMENT rather than by its own initializer
// (issue #4720; spec §28.3 K8.3/K8.4, §35.5 *The owning match*, v0.307).
//
// THE RULE. §28.3 K8.3 puts a `case` scrutinee's boundary at "the whole
// construct — pattern bindings are borrows of the scrutinee, and the
// temporary survives arm selection, guard evaluation, and the selected arm's
// completion — in statement position exactly as in expression position".
// K8.4: "a temporary is an admissible borrow source only within its
// boundary; a borrow or frame that would escape it is rejected, never
// lifetime-extended".
//
// THE BUG (#4720). #4635 closed the DIRECT expression-position channel,
// #4663 the DIRECT statement-position one, and #4715 the INITIALIZER-sourced
// indirect one (`const c := w; outer := c;` and seven siblings). #4715's
// trace reads `collectOwningMatchArmAliasInits`, which records an arm-local
// as an alias ONLY IF it is never assigned in the arm — an exclusion that is
// load-bearing in the other direction, because without it
// `var c := w; c := alt; outer := c;` (which stores `alt`, and is sound)
// would be falsely rejected. The converse therefore stayed open:
//
//   case move o of
//     W(var w, _) => do
//       var c := alt;
//       c := w;            // the payload reaches `c` by ASSIGNMENT
//       outer := c;        // admitted — `c` is in no alias map
//     end,
//     X => ()
//   end case;
//
// MEASURED on r19-c23 with #4715 landed (`6221ed38`): that program compiled
// CLEAN and leaked 24 B / 1 allocation under LSan, `dispose a` never printed.
//
// THE FIX. "`c`'s value is its initializer's" and "`c` was reassigned" are
// both program-POINT facts, and #4663's flat `forEachNestedStmt` sweep has no
// program point to hold one at. The walk is now ORDERED and carries a
// may-taint set: a store of the payload into an arm-local TAINTS it, a store
// of anything else KILLS the taint, and a store out of a tainted local is the
// escape. The two error directions are not symmetric — under-rejecting leaves
// today's leak, over-rejecting breaks a working program — so a KILL applies
// only along a straight-line sequence, and a nested CONDITIONAL contributes
// its taint by union while its kills stay inside it. Rows (3) and (4) pin
// both halves of that choice.
//
// SEVERITY. Today every REJECT row below is a leak — #4617's per-slot
// discharge leaves the bound slot out of the construct-completion reclaim, so
// the payload is merely never disposed. The instant #4634 gives that binding
// its §28.3 auto-drop each one becomes a heap-use-after-free, and the
// `return`-through-alias row (6) becomes a double-free outright: it hands the
// payload to a caller that adopts it with `using var` and disposes a value it
// never received ownership of. #4720 is the LAST open channel blocking #4634.
//
// OBSERVABILITY — why the oracle is the DIAGNOSTIC and not the leak. The
// pre-fix behaviour is a CLEAN COMPILE, so `failedAtCompileTimeWithReal
// Location` answers true for any unrelated rejection and is vacuous here. A
// leak oracle is worse than vacuous: #4715 measured that its two-hop and
// aliased-capture rows printed NO LeakSanitizer report on a one-shot run
// pre-fix — the payload stays reachable from a live stack slot / capture cell
// at exit — while never printing `dispose a` either, which is exactly
// CLAUDE.md's "one-shot leak repros pass vacuously" case. So every REJECT row
// asserts the SPECIFIC K8.4 escape diagnostic, and every ACCEPT row asserts a
// completely error-free compile (not merely the absence of that needle, which
// an unrelated syntax error would also satisfy).
//
// per §28.3 K8.3/K8.4, §35.5, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Asserting the specific text is load-bearing: the pre-fix compiler ACCEPTS
// every REJECT row below, so any weaker oracle would not distinguish the fix
// from an unrelated error.
const char* kEscapeNeedle = "owning-match payload binding";
const char* kCaptureNeedle = "captures owning-match payload binding";

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int errorCount(const JitResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

bool rejectsWith(const std::string& program, const char* needle) {
    return hasErrorContaining(runAot(program), needle);
}

// The ACCEPT oracle: the program must compile with NO error at all. A weaker
// "does not carry the escape needle" check would pass vacuously if the row
// were rejected for an unrelated reason.
bool compilesWithoutError(const std::string& program) {
    return errorCount(runAot(program)) == 0;
}

// Mirrors owning_match_alias_escape_4715_test.cpp's prelude exactly — two
// `owned` class payloads and a payload-free case, `Self()` for a
// borrow-returning chain, `Holder` for the plain-field row.
const char* kPrelude =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "  public method Self(): Widget return this; end Self; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Holder "
    "  public Held: Widget; "
    "  public constructor(w: Widget) Held := w; end constructor; "
    "end Holder; "
    "public class Program ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end Program;";
}

// A program with an extra static method ahead of `Main`, for the
// return-position row.
std::string progWith(const std::string& extra, const std::string& mainBody) {
    return std::string(kPrelude) + extra +
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end Program;";
}

// The shared setup every row below shares: two outer owners to alias, the
// destination that outlives the arm, and the owning-choice scrutinee.
const char* kSetup =
    "    using var seed := new Widget(\"seed\"); "
    "    using var alt := new Widget(\"alt\"); "
    "    var outer := seed; "
    "    var p := true; "
    "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); ";

const char* kTail =
    "    Console.WriteLine($\"outer={outer.Ping()}\"); "
    "    return 0; ";

// Wrap an arm body in the owning match the rows all share.
std::string armed(const std::string& armBody) {
    return std::string(kSetup) +
           "    case move o of "
           "      W(var w, _) => do " + armBody + " end, "
           "      X => () "
           "    end case; " +
           kTail;
}

// ===========================================================================
// REJECT rows — the assignment-sourced channel, every spelling measured
// escaping on r19-c23 (`6221ed38`).
// ===========================================================================

// (1) THE ISSUE'S REPRO, verbatim. `c` is declared from an unrelated owner
// and then ASSIGNED the payload, so it is excluded from #4715's alias map and
// the store out passed. Measured pre-fix: compiled clean, LSan direct leak of
// 24 B / 1 allocation, `dispose a` never printed.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           assignment_sourced_alias_store_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed(
        "        var c := alt; "
        "        c := w; "
        "        outer := c; ")), kEscapeNeedle));
}

// (2) A two-hop ASSIGNMENT chain. The taint has to propagate through a second
// assigned local, which is the difference between a one-statement special
// case and a real per-point fact.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           two_hop_assignment_chain_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed(
        "        var c := alt; "
        "        var d := alt; "
        "        c := w; "
        "        d := c; "
        "        outer := d; ")), kEscapeNeedle));
}

// (3) The taint is created inside a CONDITIONAL and must survive the merge:
// on the `p` path the payload really does reach `outer`, and K8.4 rejects
// statically, so a branch's taint contributes by union.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           taint_from_a_conditional_branch_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed(
        "        var c := alt; "
        "        if p then c := w; end if; "
        "        outer := c; ")), kEscapeNeedle));
}

// (4) The OTHER half of the merge rule: a KILL inside a conditional must NOT
// propagate out of it. On the `!p` path `c` still holds the payload, so the
// program escapes on one path and is correctly rejected. A walk that let the
// branch's kill clear the taint would admit this — and that is precisely the
// direction in which being wrong is a use-after-free rather than a leak.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           kill_inside_a_conditional_does_not_clear_the_taint_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed(
        "        var c := w; "
        "        if p then c := alt; end if; "
        "        outer := c; ")), kEscapeNeedle));
}

// (5) A LOOP-CARRIED taint: the store out is written BEFORE the tainting
// assignment in source order, so only a walk that reaches the loop body's
// taint fixpoint sees it. It must be reported exactly once — the fixpoint
// passes are silent so they cannot double-report.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           loop_carried_taint_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed(
        "        var c := alt; "
        "        var i := 0; "
        "        while i < 2 do "
        "          outer := c; "
        "          c := w; "
        "          i := i + 1; "
        "        end while; ")), kEscapeNeedle));
}

// (6) RETURN through an assigned alias. §35.5's severity note for this
// family: the caller adopts the returned value with `using var` and disposes
// a payload it never received ownership of — a double free the instant
// #4634's auto-drop lands, not merely a dangling read.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           assigned_alias_return_from_owned_method_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(progWith(
        "  public static method Take(o2: owned Outer, sub: Widget): "
        "      owned Widget "
        "    case move o2 of "
        "      W(var w, _) => do "
        "        var c := sub; "
        "        c := w; "
        "        return c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    return new Widget(\"fb\"); "
        "  end Take; ",
        "    using var sub := new Widget(\"sub\"); "
        "    using var got := "
        "        Take(Outer.W(new Widget(\"a\"), new Widget(\"b\")), sub); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (7) The assignment-sourced CLOSURE CAPTURE. #4663 closed the direct capture
// and #4715 the initializer-aliased one; the capture route now reads the same
// taint the direct store route does, so the two cannot drift apart.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           assigned_alias_closure_capture_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        std::string(kSetup) +
        "    var f: () -> int := lambda(): int => 0; "
        "    case move o of "
        "      W(var w, _) => do "
        "        var c := alt; "
        "        c := w; "
        "        f := lambda(): int => c.Ping(); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"f={f()}\"); " + kTail),
        kCaptureNeedle));
}

// (8) A plain FIELD store rather than a local store. The destination is a
// heap object that outlives the construct, so the boundary argument is the
// same one; this row pins that the taint reaches every store target shape,
// not just a bare outer local.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           assigned_alias_field_store_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        std::string(kSetup) +
        "    using var h := new Holder(seed); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var c := alt; "
        "        c := w; "
        "        h.Held := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"h={h.Held.Ping()}\"); " + kTail),
        kEscapeNeedle));
}

// ===========================================================================
// ACCEPT rows — the companion half. A taint set without KILLS, or one that
// ignored what an assignment actually stored, would fail these while rows
// 1-8 still passed. They are what makes the fix's breadth measurable rather
// than asserted, and (A1) is #4715's own shipped ACCEPT row A6, carried here
// unchanged because it is the exact program this fix must not break.
// ===========================================================================

// (A1) #4715's ACCEPT row A6. The alias is initialized FROM the binding and
// then reassigned before the store, so what escapes is `alt`, not the
// payload. The kill is what keeps this accepted.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           reassigned_alias_stores_other_value_accepted_35_5) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(armed(
        "        var c := w; "
        "        c := alt; "
        "        outer := c; "))));
}

// (A2) Taint then KILL then store, in one straight-line sequence. The value
// that reaches `outer` is `alt`; a taint set that only ever grew would reject
// this program, which is sound.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           taint_then_kill_then_store_accepted_35_5) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(armed(
        "        var c := alt; "
        "        c := w; "
        "        c := alt; "
        "        outer := c; "))));
}

// (A3) The tainted alias is READ only within the arm's own block. §35.5
// admits exactly this — the payload is alive through the arm — so the taint
// must gate the STORE OUT, never the read.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           tainted_alias_read_inside_the_arm_accepted_35_5) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(armed(
        "        var c := alt; "
        "        c := w; "
        "        Console.WriteLine($\"c={c.Ping()}\"); "))));
}

// (A4) The store's TARGET is itself a local declared inside the arm, so it
// dies at the arm's own `end` — inside K8.3's boundary. The value side IS
// tainted here, so the row proves the arm-scoped-target exemption still runs
// after the trace rather than being bypassed by it.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           tainted_value_into_arm_scoped_target_accepted_28_3_k8_3) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(armed(
        "        var t := alt; "
        "        var c := alt; "
        "        c := w; "
        "        t := c; "
        "        Console.WriteLine($\"t={t.Ping()}\"); "))));
}

// (A5) An arm-local of NON-borrowable type reassigned in the arm. An `int`
// can carry no borrow, so the type gate must stop the trace before the taint
// machinery ever runs — otherwise every arm with a loop counter would lose
// its accepted status.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           reassigned_value_typed_arm_local_accepted_28_3) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(armed(
        "        var q := 0; "
        "        q := q + 1; "
        "        Console.WriteLine($\"n={w.Ping() + q}\"); "))));
}

// (A6) A BORROWING match — §35.5's "the construct never moves from the
// scrutinee". Nothing is transferred, so no binding can escape and the
// assignment-sourced alias store is ordinary borrowing.
BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           borrowing_match_assigned_alias_store_accepted_35_5) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    using var alt := new Widget(\"alt\"); "
        "    var outer := seed; "
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => do "
        "        var c := alt; "
        "        c := w; "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; ")));
}

// ===========================================================================
// CONTROL rows — the channels #4635 / #4663 / #4715 already close. They pin
// that the ordered walk carried their decisions over verbatim rather than
// re-deriving them.
// ===========================================================================

BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           control_direct_store_still_rejected_4663) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed("        outer := w; ")),
                                  kEscapeNeedle));
}

BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           control_initializer_alias_still_rejected_4715) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(armed(
        "        const c := w; "
        "        outer := c; ")), kEscapeNeedle));
}

BEZEL_TEST(owning_match_assigned_alias_escape_4720,
           control_direct_expression_arm_still_rejected_4635) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of W(var w, _) => w, X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

}  // namespace
