// K8.4 at the §35.5 owning-match boundary — the INDIRECT channel: a store or
// return routed through an intermediate same-arm local alias (issue #4715;
// spec §28.3 K8.3/K8.4, §35.5 *The owning match*, §17.15, v0.307).
//
// THE RULE. §28.3 K8.3 puts a `case` scrutinee's boundary at "the whole
// construct — pattern bindings are borrows of the scrutinee, and the
// temporary survives arm selection, guard evaluation, and the selected arm's
// completion — in statement position exactly as in expression position".
// K8.4: "a temporary is an admissible borrow source only within its
// boundary; a borrow or frame that would escape it is rejected, never
// lifetime-extended".
//
// THE BUG (#4715). #4635 closed the DIRECT expression-position channel
// (`W(var w, _) => w`) and #4663 the DIRECT statement-position one
// (`do outer := w; end`) plus the direct closure capture. Both resolved the
// escaping value with `identifierRoot`, which is a ONE-HOP peel: given a bare
// identifier it returns that identifier and stops. So an alias defeats both:
//
//   case move o of
//     W(var w, _) => do
//       const c := w;      // or `var c := w;` — identical result
//       outer := c;        // `identifierRoot` roots at `c`, never in
//     end,                 // `transferred` (only `w` is) — store admitted
//     X => ()
//   end case;
//
// MEASURED on r19-c23 (`92f2f29e`, both prior fixes landed) — every one of
// the eight REJECT rows below compiled CLEAN, while its DIRECT spelling was
// rejected. Widening the arm's `localDeclsInArm` set does NOT close it: that
// set exempts an assignment's own TARGET, and `c` here is on the VALUE side
// of a different assignment.
//
// SEVERITY. Today this is a leak — #4617's per-slot discharge leaves the
// bound slot out of the construct-completion reclaim, so the payload is
// merely never disposed. The instant #4634 gives that binding its §28.3
// auto-drop, every row below becomes a heap-use-after-free, and the
// `return`-through-alias row (6) becomes a double-free outright: it hands the
// payload to a caller that adopts it with `using var` and disposes a value it
// never received ownership of.
//
// OBSERVABILITY — why the oracle is the DIAGNOSTIC and not the leak.
// The pre-fix behaviour is a CLEAN COMPILE, so `failedAtCompileTimeWith
// RealLocation` would answer true for any unrelated rejection and is vacuous
// here. A leak oracle is worse than vacuous: rows (3) two-hop and (7) aliased
// capture printed NO LeakSanitizer report on a one-shot run pre-fix — the
// payload is still reachable from a live stack slot / capture cell at exit —
// while never printing `dispose a` either. That is exactly CLAUDE.md's
// "one-shot leak repros pass vacuously" case. So every REJECT row asserts the
// SPECIFIC K8.4 escape diagnostic, and every ACCEPT row asserts a completely
// error-free compile (not merely the absence of that needle, which an
// unrelated syntax error would also satisfy).
//
// per §28.3 K8.3/K8.4, §35.5, §17.15, v0.307

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

// Mirrors owning_match_stmt_escape_4663_test.cpp's prelude exactly — two
// `owned` class payloads and a payload-free case, `Self()` for the
// borrow-returning-chain row, `Holder` for the plain-field row.
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

// A program with an extra static method ahead of `Main`, for the two
// return-position rows.
std::string progWith(const std::string& extra, const std::string& mainBody) {
    return std::string(kPrelude) + extra +
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end Program;";
}

// ===========================================================================
// REJECT rows — the indirect channel, every spelling measured escaping.
// ===========================================================================

// (1) THE ISSUE'S REPRO, verbatim: a `const` intermediate between the
// transferred binding and the store out. Measured pre-fix: compiled clean,
// LSan direct leak of 24 B / 1 allocation, `dispose a` never printed.
BEZEL_TEST(owning_match_alias_escape_4715,
           const_alias_store_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := w; "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (2) The `var` spelling of the same intermediate. #4715 measured both
// spellings leaking identically, and the fix must not be `const`-only — the
// arm's existing `localDeclsInArm` set IS LocalDecl-only, for an unrelated
// structural reason (§21.1), which is exactly the confusion #4715 records.
BEZEL_TEST(owning_match_alias_escape_4715,
           var_alias_store_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var c := w; "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (3) A chain of TWO intermediates — #4715 asks for this explicitly, to prove
// the trace is genuinely recursive and not accidentally bounded to one extra
// hop. Pre-fix this row printed no LSan report at all (the payload stayed
// reachable from a live stack slot) while still never printing `dispose a`,
// which is why the oracle here is the diagnostic.
BEZEL_TEST(owning_match_alias_escape_4715,
           two_hop_alias_chain_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const a1 := w; "
        "        const b1 := a1; "
        "        outer := b1; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (4) The alias is initialized from a BORROW-RETURNING CHAIN rooted at the
// binding (`w.Self()`) rather than from the binding itself — #4635's own
// row (4) shape, one hop further out.
BEZEL_TEST(owning_match_alias_escape_4715,
           alias_of_borrow_chain_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := w.Self(); "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (5) The escaping LOCATION is a plain (non-`owned`) FIELD, not a local —
// #4663's own breadth note, through an alias. A store into an `owned` field
// draws a different, pre-existing rejection; the plain field has no other
// guard.
BEZEL_TEST(owning_match_alias_escape_4715,
           alias_store_to_plain_field_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var h := new Holder(seed); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := w; "
        "        h.Held := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"h={h.Held.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (6) RETURN position, through an alias, out of a method declared `owned
// Widget`. The most severe row: pre-fix it compiled clean and RAN, printing
// `dispose b / got=7 / dispose a` — the caller's `using var` disposed a
// payload it never received ownership of. It balances today only because
// #4634's leak means the arm never drops it; #4634 turns this into a
// double-free. The DIRECT spelling (`return w;`) is rejected by #4663 AND by
// the general "a borrow cannot be returned as an `owned` value" check; the
// aliased one escaped both, because that general check tracks `var`-local
// ownership state and not a `const` local's.
BEZEL_TEST(owning_match_alias_escape_4715,
           alias_return_from_owned_method_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(progWith(
        "  public static method Take(o2: owned Outer): owned Widget "
        "    case move o2 of "
        "      W(var w, _) => do "
        "        const c := w; "
        "        return c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    return new Widget(\"fb\"); "
        "  end Take; ",
        "    using var got := "
        "        Take(Outer.W(new Widget(\"a\"), new Widget(\"b\"))); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (7) The ALIASED CLOSURE CAPTURE. #4663 closed the DIRECT capture
// (`lambda(): int => w.Ping()`); the lambda capturing the alias `c` instead
// was still admitted, because that check compared each captured identifier's
// symbol against `transferred` with no alias trace of its own. Pre-fix this
// row also printed no LSan report (the payload is held by a live capture
// cell) while never printing `dispose a`.
BEZEL_TEST(owning_match_alias_escape_4715,
           aliased_closure_capture_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    var f: () -> int := lambda(): int => 0; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := w; "
        "        f := lambda(): int => c.Ping(); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"f={f()}\"); "
        "    return 0; "), kCaptureNeedle));
}

// (8) EXPRESSION position. An expression-position arm declares no statements,
// so its alias spelling is §17.15's binding atom `(var c := w; c)` —
// and `identifierRoot` peels a `BindExpr` straight to its body, landing on
// `c`. #4635's check therefore had the identical hole in its own position.
BEZEL_TEST(owning_match_alias_escape_4715,
           expression_position_binding_atom_alias_rejected_17_15) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of "
        "                  W(var w, _) => (var c := w; c), "
        "                  X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// ===========================================================================
// ACCEPT rows — the companion half. An over-broad trace (one that rejected
// any arm-local whose name reaches a store, or that ignored what the alias
// was actually initialized from) would fail these while rows 1-8 still
// passed, so they are what makes the fix's breadth measurable rather than
// asserted. All six were measured ACCEPTED both before and after the fix.
// ===========================================================================

// (A1) The alias is initialized from an UNRELATED outer local, not from the
// transferred binding. The arm binds `w` and the store is of `alt`.
BEZEL_TEST(owning_match_alias_escape_4715,
           alias_of_unrelated_outer_local_accepted) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    using var alt := new Widget(\"alt\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := alt; "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; ")));
}

// (A2) An alias CHAIN off the transferred binding that is never stored out at
// all. Reading the payload inside the arm is exactly what §35.5 admits, and a
// `LocalDecl`'s own initializer is deliberately not a store — the declared
// local dies at the arm's own `end`, inside K8.3's boundary.
BEZEL_TEST(owning_match_alias_escape_4715,
           alias_chain_never_stored_out_accepted_35_5) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := w; "
        "        const d := c; "
        "        Console.WriteLine($\"d={d.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    return 0; ")));
}

// (A3) A VALUE-typed alias of a read through the binding. `w.Ping()` yields
// an `int`; storing a copy of it out carries no borrow, so the type gate must
// still stop the trace before it ever runs.
BEZEL_TEST(owning_match_alias_escape_4715,
           value_typed_alias_stored_out_accepted_28_3) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    var n := 0; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        const c := w.Ping(); "
        "        n := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; ")));
}

// (A4) The store's TARGET is itself a local declared inside the arm, so it
// dies with the arm's own block — inside K8.3's boundary. The value side
// does root at `w` through the alias here, so this row proves the
// target-exemption still runs after the trace rather than being bypassed by
// it.
BEZEL_TEST(owning_match_alias_escape_4715,
           alias_stored_into_arm_scoped_target_accepted_28_3_k8_3) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var t := w; "
        "        const c := w; "
        "        t := c; "
        "        Console.WriteLine($\"t={t.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    return 0; ")));
}

// (A5) A BORROWING match (no `move`, no unbound owned temporary) — §35.5's
// "the construct never moves from the scrutinee". Nothing is transferred, so
// no binding can escape and the alias store is ordinary borrowing. Measured
// running clean (`dispose a / dispose b / dispose seed`, no LSan report).
BEZEL_TEST(owning_match_alias_escape_4715,
           borrowing_match_alias_store_accepted_35_5) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => do "
        "        const c := w; "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; ")));
}

// (A6) The alias is initialized from the binding but REASSIGNED before the
// store, so the value that escapes is `alt`, not the payload. This is the row
// that fixes the trace's shape: it is why an arm-local assigned anywhere in
// the arm is excluded from the alias map rather than traced unconditionally.
// Measured running clean (`dispose b / outer=7 / dispose alt / dispose seed`).
BEZEL_TEST(owning_match_alias_escape_4715,
           reassigned_alias_stores_other_value_accepted) {
    BEZEL_EXPECT_TRUE(compilesWithoutError(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    using var alt := new Widget(\"alt\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        var c := w; "
        "        c := alt; "
        "        outer := c; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; ")));
}

// ===========================================================================
// CONTROL rows — the DIRECT spellings #4635/#4663 already reject. They pin
// that this fix did not disturb the one-hop path it is built on top of: the
// trace's FIRST hop is exactly the pre-#4715 comparison.
// ===========================================================================

BEZEL_TEST(owning_match_alias_escape_4715,
           control_direct_store_still_rejected_4663) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var seed := new Widget(\"seed\"); "
        "    var outer := seed; "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do outer := w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine($\"outer={outer.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

BEZEL_TEST(owning_match_alias_escape_4715,
           control_direct_expression_arm_still_rejected_4635) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of W(var w, _) => w, X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

}  // namespace
