// `move` of a case-arm pattern binding the match does not transfer is a
// double free (issue #4664; spec §35.5 *The borrowing match*, §19.3,
// §28.3 K8.3/K3.1, v0.307).
//
// THE RULE. §35.5 *The borrowing match*: "Matching a choice value
// non-destructively is the default everywhere, and §19.3's rules apply
// verbatim: a pattern binding is a borrow whose source is the scrutinee's
// source; the scrutinee retains its ownership state; the construct never
// moves from the scrutinee." And, load-bearing for the repro: "An unmarked
// read of a named owned binding in scrutinee position stays a borrowing
// match." Only §35.5's two ANNOUNCING scrutinee forms — an unbound owned
// temporary of owning-choice classification, and a named owned binding under
// the explicit `case move o of` marker — transfer a payload into an arm
// binding; every other scrutinee leaves the binding a borrow. §28.3's consume
// table then forbids transferring one: a borrow has no ownership to hand on
// (K3.1's family-1 transfers take an OWNING source), and K8.3 keeps the
// scrutinee live through the selected arm's completion.
//
// THE BUG (#4664). The checker admitted the transfer. MEASURED on HEAD
// e71f9644, under `--sanitize=address`:
//
//   using var o := Outer.W(new Widget("a"), new Widget("b"));
//   case o of                                   // BORROWING match (§35.5)
//     W(var w, _) => do using var stolen := move w; …  end,
//     X => ()
//   end case;
//
// compiled clean and ran as a heap-use-after-free: it printed `live=7`,
// `dispose a` (the arm's `using var` disposing the payload at arm exit),
// `after`, and then `o`'s own scope-end active-case pass (§35.4) disposed the
// same payload a second time. The ownership dataflow keeps no `states_` entry
// for a pattern binding, so `markMovedByName`'s `if (!st) return;` accepted
// the consume silently and `producerState` classified the `move` result Owned
// through its plain-class fallback — a second owner for a value the scrutinee
// still owns.
//
// The same fault was measured for every reject row below. The already-closed
// neighbours are deliberately NOT re-pinned here: an expression-position arm
// result `=> move w` was already rejected by `consumeMarkedArmTransfer`
// ("requires a named owned binding"), and `Consume(move w)` into an `owned`
// parameter by §28.8's borrow-into-owned-sink check. Only the binding and
// assignment positions were open.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN COMPILE followed by a
// runtime fault, so the oracle on every reject row is the SPECIFIC diagnostic
// text — never `failedAtCompileTimeWithRealLocation` (a crash's own backstop
// diagnostic carries a real location, so it answers true on both sides) and
// never a bare `!r.ran` (runAot synthesizes an Error for a runtime crash too).
// The needle below does not exist in the pre-fix compiler, so every reject row
// is red without the fix.
//
// per §35.5, §19.3, §28.3 K8.3/K3.1, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The exact diagnostic this fix introduces. Asserting the specific text is
// load-bearing: the pre-fix compiler ACCEPTS these programs, so any weaker
// oracle would not distinguish the fix from an unrelated rejection.
const char* kNeedle = "cannot `move` the `case` arm binding";

// §35.5's own owning-choice shape: two `owned` class payloads and a
// payload-free case, plus a record carrying a class-typed field for the
// record-destructure row.
const char* kDecls =
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public record Pair(w: Widget, n: int); ";

std::string prog(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program public static method Main(): int " + body +
           " end Main; end Program;";
}

// A variant carrying an extra static helper before `Main` (the borrowed-
// scrutinee row needs a method whose parameter is the scrutinee).
std::string progWith(const std::string& helper, const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program " + helper +
           " public static method Main(): int " + body +
           " end Main; end Program;";
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    return hasErrorContaining(r, needle);
}

void expectRun(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// --- REJECT rows: the transfer out of a borrow -----------------------------

// (1) THE ISSUE'S REPRO, verbatim: §35.5's borrowing match over a `using var`
// named owned binding, an arm that binds payload `a`, and a `using var`
// adoption of `move w` inside the arm's `do … end` body.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_using_var_adoption_of_arm_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => do "
        "        using var stolen := move w; "
        "        Console.WriteLine($\"live={stolen.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (2) The same over a PLAIN `var` owned local. §35.5's borrowing-match rule
// keys on the scrutinee FORM (an unmarked named binding), not on how the
// binding's own disposal is scheduled, so the `using var` of row 1 is not
// what makes the transfer illegal.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_plain_var_scrutinee_arm_binding_move_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => do "
        "        using var stolen := move w; "
        "        Console.WriteLine($\"live={stolen.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (3) The NAKED binding spelling `W(w, _)` (§19.3's Identifier alternative,
// whose LocalSymbol anchors on #1271's synthesized BindingPattern). Same
// borrow, same rejection — the check must not key on the `var` spelling.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_19_3_naked_binding_spelling_move_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(w, _) => do "
        "        using var stolen := move w; "
        "        Console.WriteLine($\"live={stolen.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (4) A plain `var` adoption with an explicit `Dispose()` — the same double
// free reached without `using var` at all, so the rejection is of the
// TRANSFER, not of the scope-bound disposal it feeds.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_28_3_k3_1_plain_local_adoption_of_arm_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => do var live := move w; live.Dispose(); end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (5) The ASSIGNMENT position — `slot := move w`. §28.3 applies the K8.3
// boundary "in statement position exactly as in expression position", and the
// measured fault is identical: the payload escapes the construct into a slot
// whose own disposal then races the scrutinee's.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_28_3_k8_3_assignment_of_arm_binding_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var slot: Widget? := null; "
        "    case o of "
        "      W(var w, _) => do slot := move w; end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (6) A BORROWED scrutinee of another provenance — a plain parameter. §35.5's
// "What does not trigger an owning match" lists "a borrowed scrutinee of any
// provenance (field read, parameter, `this`)", so the arm binding is a borrow
// there too and the caller's `using var` is the sole owner.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_borrowed_parameter_scrutinee_arm_binding_move_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(progWith(
        "  public static method Peek(v: Outer) "
        "    case v of "
        "      W(var w, _) => do "
        "        using var stolen := move w; "
        "        Console.WriteLine($\"live={stolen.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "  end Peek; ",
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    Peek(o); "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (7) A non-destructuring arm binding — §19.3's typed `var y: Widget` type
// test over a borrowed class scrutinee. §19.3's rule is stated for pattern
// bindings generally, not for choice destructures alone, and the measured
// fault is the same use-after-free.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_19_3_type_test_arm_binding_move_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    case other of "
        "      var y: Widget => do "
        "        using var stolen := move y; "
        "        Console.WriteLine($\"live={stolen.Ping()}\"); "
        "      end, "
        "      _ => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (8) A RECORD destructure's class-typed slot binding. §19.3 again: "a
// pattern binding is a borrow whose source is the scrutinee's source" — a
// record destructure routes no ownership either, and the `using var other`
// remains the payload's only owner.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_19_3_record_destructure_binding_move_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var p := new Pair(other, 1); "
        "    case p of "
        "      Pair(var q, _) => do "
        "        using var stolen := move q; "
        "        Console.WriteLine($\"live={stolen.Ping()}\"); "
        "      end "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "), kNeedle));
}

// (9) The MOVE-ONLY VALUE payload twin, through the scrutinee-marker
// position. §35.5: "a binding of a payload of move-only value type (§15.3 —
// an owning inline array, or a nested owning choice) is likewise a borrow …
// consuming operations through it are rejected — the binding does not own."
// The inline-array half is #1665/#1745's sema rule; the nested-owning-choice
// half had no check anywhere, and `case move i of` through the binding was
// measured as the same use-after-free as the class-payload rows above.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_move_only_choice_binding_scrutinee_marker_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string("module M; ") +
        "public class Widget "
        "  private _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
        "  public method Ping(): int => 7; "
        "end Widget; "
        "public choice Inner A(owned w: Widget), B; end Inner; "
        "public choice Outer W(owned i: Inner), X; end Outer; "
        "public class Program public static method Main(): int "
        "    using var o := Outer.W(Inner.A(new Widget(\"a\"))); "
        "    case o of "
        "      W(var i) => do "
        "        case move i of "
        "          A(var w) => do "
        "            using var live := move w; "
        "            Console.WriteLine($\"live={live.Ping()}\"); "
        "          end, "
        "          B => () "
        "        end case; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "
        " end Main; end Program;", kNeedle));
}

// --- ADMIT rows: §35.5's owning match keeps its transfer -------------------

// (9) §35.5's SECOND announcing scrutinee form, with the section's own
// adoption idiom (`using var resp := move r;` in a `do … end` arm). The
// binding IS an owned local of the arm body's scope here, so the transfer is
// the spec's prescribed spelling and must stay admitted. `b` is the
// un-transferred remainder, disposed at construct completion.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_marked_scrutinee_owning_match_transfer_still_admitted) {
    auto r = runAot(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        using var live := move w; "
        "        Console.WriteLine($\"live={live.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNeedle));
    expectRun(r, std::string("live=7\ndispose a\ndispose b\nafter\n"));
}

// (10) §35.5's FIRST announcing scrutinee form — an unbound owned temporary
// of owning-choice classification (§35.3's case construction mints one).
// Same transfer, same admission.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_unbound_temporary_owning_match_transfer_still_admitted) {
    auto r = runAot(prog(
        "    case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
        "      W(var w, _) => do "
        "        using var live := move w; "
        "        Console.WriteLine($\"live={live.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNeedle));
    expectRun(r, std::string("live=7\ndispose a\ndispose b\nafter\n"));
}

// (12) The move-only-choice twin's PRECISION row: the same nested shape with
// the OUTER match announcing its consume. §35.5 transfers the `owned Inner`
// payload into `i`, so `case move i of` names an owned binding and the whole
// chain stays admitted — this is the shape #4469 and #4609 already pin, and
// the row exists so row (9)'s widening cannot swallow it.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_move_only_choice_binding_of_owning_match_still_admitted) {
    auto r = runAot(
        std::string("module M; ") +
        "public class Widget "
        "  private _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
        "  public method Ping(): int => 7; "
        "end Widget; "
        "public choice Inner A(owned w: Widget), B; end Inner; "
        "public choice Outer W(owned i: Inner), X; end Outer; "
        "public class Program public static method Main(): int "
        "    var o := Outer.W(Inner.A(new Widget(\"a\"))); "
        "    case move o of "
        "      W(var i) => do "
        "        case move i of "
        "          A(var w) => do "
        "            using var live := move w; "
        "            Console.WriteLine($\"live={live.Ping()}\"); "
        "          end, "
        "          B => () "
        "        end case; "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "
        " end Main; end Program;");
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNeedle));
    expectRun(r, std::string("live=7\ndispose a\nafter\n"));
}

// (11) The borrowing match's ADMITTED use of the same binding: a plain read.
// §35.5 — "reads through the binding are admitted"; only the consuming
// operation is rejected. Both payloads are disposed once, AFTER the
// construct, by §35.4's scope-end active-case pass.
BEZEL_TEST(borrowing_match_arm_binding_move_4664,
           spec_35_5_borrowing_match_read_through_binding_still_admitted) {
    auto r = runAot(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case o of "
        "      W(var w, _) => Console.WriteLine(w.Ping()), "
        "      X           => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kNeedle));
    expectRun(r, std::string("7\nafter\ndispose a\ndispose b\n"));
}

}  // namespace
