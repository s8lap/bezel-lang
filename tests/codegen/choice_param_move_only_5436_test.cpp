// Codegen regression tests for issue #5436: §15.3 *Parameter passing* —
// **a move-only V's argument is in moved-from state after the call** — for the
// owning-classified CHOICE, the move-only kind the call-argument gate had never
// covered. Spec v0.314.
//
// §15.3 *Parameter passing*: "A parameter declared `(x: V)` for an ordinary V
// is by value (copy); for a move-only V, it is by value-move (transfer
// ownership). **The caller's argument expression is in moved-from state after
// the call.**" §35.4: "Owning choices are move-only value types (§15.3). Every
// §15.3 rule applies as written … parameter passing is by value-move". The
// bullet is stated over "a move-only V" and names no kind, so BOTH move-only
// kinds take it.
//
// THE BUG. Both call-argument gates — `recordMovesInArgs` (a method call) and
// its `new T(...)` constructor twin — admitted only
// `isMoveOnlyInlineArrayType(pt)`, the INLINE-ARRAY move-only kind. An owning
// CHOICE formal fell through to `recordMovesIn`, which has no arm for a plain
// (non-`move`-wrapped) `IdentifierExpr`, so the caller's binding was left
// fully live: `var d := Pass(b);` left `b` owning a payload `d` also owned, and
// `b`'s scope-end active-case pass disposed it a second time.
//
// MEASURED on this issue's own repro at `r23-c28` (6f341713), the method-call
// form: `dispose b` / `moved` / then SIGSEGV, and under `--sanitize=address`
// `AddressSanitizer: heap-use-after-free ... READ of size 8`. The inline-array
// twin at the same source line was already correct (#4677), which is the
// control that isolates the defect to the missing choice arm: the same program
// with an owning inline array disposes exactly once and exits 0.
//
// §15.3 spells the argument UNMARKED — "the argument is written unmarked —
// `Take(arr)`, not `Take(move arr)` — including where it is a named binding the
// call leaves moved-from" — so every program below passes the local bare. The
// `move`-marked form is the §22.3 error #5388 (v0.314) landed on 2026-10-04.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A destructor that PRINTS its name, so a row pins both the fact and the
// moment of every disposal — a doubled disposal is a duplicated line, not an
// indistinguishable crash.
const char* kNamedResourceDecls =
    "class Resource "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

// A COUNTING destructor for the loop-amplified row: a one-shot double free can
// pass on both sides of a fix (the second dispatch may land in still-mapped
// memory), so the strong oracle is the exact count over many iterations.
const char* kCountingResourceDecls =
    "class Resource "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource._n := Resource._n + 1; end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body,
                           const std::string& extraMembers = "") {
    return runAot(std::string("module M; ") + decls +
                  " class Host public static method Main() " + body +
                  " end Main; " + extraMembers + " end Host;");
}

// `ran` and `exitCode` are asserted alongside the text because `runAot`
// reports a signal death as `ran == false` / `exitCode == -1` — which is
// exactly the pre-fix observable for these inputs.
void expectRunOutput(const bezel::test::JitResult& r,
                     const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- The method-call form: the issue's own repro ----------------------------

BEZEL_TEST(codegen_choice_param_move_only_5436,
           spec_15_3_move_only_choice_argument_is_moved_from_after_a_method_call) {
    // §15.3 *Parameter passing*, verbatim shape. The payload is constructed
    // once, moved into `Pass`, and comes back out through `d`; exactly one
    // disposal, at `d`'s scope end. Pre-fix: `dispose b` / `moved` / SIGSEGV —
    // `b`'s scope-end pass ran on a payload `d` already owned.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var b := Bundle.Ready(new Resource(\"b\")); "
            "var d := Host.Pass(b); "
            "Console.WriteLine(\"moved\");",
            "public static method Pass(v: Bundle): Bundle => v;"),
        std::string("moved\ndispose b\n"));
}

// --- The loop-amplified count: a one-shot double free can pass --------------

BEZEL_TEST(codegen_choice_param_move_only_5436,
           spec_15_3_every_move_only_choice_argument_in_a_loop_is_disposed_once) {
    // 500 iterations, one `Resource` each, every one moved into `Sink` and out
    // to the loop-local `d`, which is re-bound each round. §35.4's
    // drop-on-overwrite disposes the 499 values `d` displaces and the scope-end
    // pass disposes the 500th, so the spec's count is exactly 500 — one per
    // construction. A count ABOVE 500 is the double free; BELOW it is a
    // suppression that has grown into a leak. The loop lives in its own method
    // so the scope-end disposal of the final value has run when the count
    // prints.
    expectRunOutput(
        run(kCountingResourceDecls,
            "Host.Churn(); Console.WriteLine(Resource._n);",
            "public static method Churn() "
            "  var d := Bundle.Empty; "
            "  var i := 0; "
            "  while i < 500 do "
            "    var b := Bundle.Ready(new Resource()); "
            "    d := Host.Pass(b); "
            "    i := i + 1; "
            "  end while; "
            "end Churn; "
            "public static method Pass(v: Bundle): Bundle => v;"),
        // 499 displaced + 1 scope-end disposal.
        std::string("500\n"));
}

// --- The `new T(...)` constructor twin --------------------------------------

BEZEL_TEST(codegen_choice_param_move_only_5436,
           spec_15_3_move_only_choice_argument_is_moved_from_after_a_ctor_call) {
    // The same rule at the constructor gate — `recordMovesInArgs`' own twin,
    // which #4677 established as a separate site when it added the
    // inline-array arm and left the choice one out. MEASURED pre-fix: this
    // shape is already memory-SOUND (the ctor's own field store disposes the
    // displaced value correctly), so the row is a REJECTION row: §15.3 still
    // requires the caller's argument to read as moved-from, and pre-fix it did
    // not. The read is what makes this a spec-derived rejection rather than a
    // duplicate of the row above.
    auto r = runAot(
        "module M; "
        "class Resource public constructor() end constructor; "
        "  destructor end destructor; end Resource; "
        "choice Bundle Ready(owned r: Resource), Empty; end Bundle; "
        "class Wrap private _v: Bundle; "
        "  public constructor(v: Bundle) _v := v; end constructor; "
        "end Wrap; "
        "class Host public static method Main() "
        "  var b := Bundle.Ready(new Resource()); "
        "  var w := new Wrap(b); "
        "  var e := b; "          // §15.3: `b` was moved-from by the call
        "  Console.WriteLine(\"unreachable\"); "
        "end Main; end Host;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawMovedOut = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("already moved out") != std::string::npos)
            sawMovedOut = true;
    BEZEL_EXPECT_TRUE(sawMovedOut);
}

// --- A value-like choice is NOT move-only and must stay unmarked ------------

BEZEL_TEST(codegen_choice_param_move_only_5436,
           spec_35_4_value_like_choice_argument_is_copied_not_moved) {
    // §35.4 rule 3: a choice with no `owned` payload and no borrow is
    // value-like, "an ordinary copyable value type" — §15.3's move-only rules
    // do not reach it, so the source stays readable after the call. This is
    // the negative control that keeps the widened gate from over-marking: the
    // fix widens WHICH KINDS are admitted, never all choices. `v` is read
    // through a match after `Make(7)`, so an over-wide gate would reject this
    // row outright.
    expectRunOutput(
        run("choice Outcome Fetched(x: int), Failed; end Outcome; ",
            "var v := Host.Make(7); "
            "case v of "
            "  Fetched(n) => Console.WriteLine($\"{n}\"), "
            "  Failed => Console.WriteLine(\"failed\"), "
            "end case;",
            "public static method Make(x: int): Outcome "
            "  => Outcome.Fetched(x);"),
        std::string("7\n"));
}