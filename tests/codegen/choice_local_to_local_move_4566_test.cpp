// Codegen regression tests for issue #4566: §15.3 *Move-only value types* —
// **Local-to-local assignment** — and §28.7 *Moved-from and null*, spec v0.306.
//
// §15.3: "A move-only value type is a value type whose instances cannot be
// duplicated. Assignment transfers the value from source to destination,
// leaving the source in moved-from state." Its first *Rules* bullet spells the
// case out: "**Local-to-local assignment** `var y := x;`: copies for an
// ordinary value type; moves for a move-only value type. After the move, `x`
// is in moved-from state" — unmarked, because "a move-only value binding has
// no borrow reading to disambiguate". An owning-classified choice is one of
// the two move-only kinds (§15.3, §35.4).
//
// §15.3's *Local lifetime* bullet states what the move costs the SOURCE: a
// whole-value move "leaves the local holding no value rather than a hole, so
// no pass walks it (K1's moved-from state: 'discharge machinery skips it';
// §28.3's auto-drop runs only on a local 'not in moved-from state')". §28.7
// says the same for the next store into it: "A moved-from or null location
// therefore stores without disposal."
//
// THE BUG. Codegen implemented only the store half. `c := b;` and `var c := b;`
// plain-copied the choice struct, so BOTH slots held the same owned payload
// pointer and BOTH scope-end active-case passes dispatched `Dispose` through
// it — a double free. Measured on the issue's own repro at `origin/main`:
// `dispose c` / `moved` / `dispose b` then SIGSEGV (the second dispatch read a
// freed vtable), and `AddressSanitizer: heap-use-after-free ... READ of size 8`
// under `--sanitize=address`. The same gap made a store into the moved-from
// source (`c := b; b := Bundle.Ready(new Resource("b2"));`) dispose the payload
// its new owner then disposed again.
//
// THE ORACLE. Each row pins the exact DISPOSAL COUNT, not "did not crash": a
// destructor that prints its name gives one line per disposal, so a doubled
// disposal and a suppressed one are distinguishable from each other and from
// the spec's answer. `ran` and `exitCode` are asserted alongside the text
// because `runAot` reports a signal death as `ran == false` / `exitCode == -1`,
// which is exactly the pre-fix observable for these inputs.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A destructor that PRINTS, so a row can pin both the fact and the moment of
// every disposal, and a doubled disposal shows up as a duplicated line rather
// than as an indistinguishable crash.
const char* kNamedResourceDecls =
    "class Resource "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

// A COUNTING destructor for the loop-amplified row: a one-shot double-free can
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

// Assert the program RAN to a clean exit and produced exactly `expected` on
// stdout. Split out because every row needs all three assertions: a pre-fix
// double free is a signal death, which `runAot` reports as ran == false with
// an empty-or-truncated stdout.
void expectRunOutput(const bezel::test::JitResult& r,
                     const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- §15.3 bullet 1: the assignment form ------------------------------------

BEZEL_TEST(codegen_choice_local_to_local_move_4566,
           spec_15_3_local_to_local_assignment_moves_the_owning_choice) {
    // The issue's repro verbatim. §35.4's drop-on-overwrite disposes `c`'s
    // displaced value at the store (#4552), §15.3's move then leaves `b`
    // holding nothing, and the single surviving value — the one `c` now owns —
    // is disposed once at scope end. Pre-fix: the same three lines followed by
    // SIGSEGV, because `b`'s slot still held the pointer `c` had just freed.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var b := Bundle.Ready(new Resource(\"b\")); "
            "var c := Bundle.Ready(new Resource(\"c\")); "
            "c := b; "
            "Console.WriteLine(\"moved\");"),
        std::string("dispose c\nmoved\ndispose b\n"));
}

// --- §15.3 bullet 1: the binding form, which is the spec's own spelling -----

BEZEL_TEST(codegen_choice_local_to_local_move_4566,
           spec_15_3_var_binding_from_a_local_moves_the_owning_choice) {
    // §15.3 states the rule as "`var y := x;`", so the declaration form is the
    // bullet's own example and not a widening of it. It double-freed exactly
    // as the `:=` form did: pre-fix `moved` / `dispose b` / SIGSEGV.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var b := Bundle.Ready(new Resource(\"b\")); "
            "var c := b; "
            "Console.WriteLine(\"moved\");"),
        std::string("moved\ndispose b\n"));
}

// --- §28.7 *Moved-from and null*: the next store disposes nothing -----------

BEZEL_TEST(codegen_choice_local_to_local_move_4566,
           spec_28_7_store_into_a_moved_from_owning_choice_local_drops_nothing) {
    // "A moved-from or null location therefore stores without disposal."
    // After `c := b;` the payload belongs to `c`, so `b := Bundle.Ready(...)`
    // must NOT run §35.4's drop-on-overwrite on the stale bytes. Three
    // resources are constructed and each is disposed exactly once. Pre-fix:
    // `dispose c` / `dispose b` (the wrong drop) / `reacquired` / SIGSEGV.
    expectRunOutput(
        run(kNamedResourceDecls,
            "var b := Bundle.Ready(new Resource(\"b\")); "
            "var c := Bundle.Ready(new Resource(\"c\")); "
            "c := b; "
            "b := Bundle.Ready(new Resource(\"b2\")); "
            "Console.WriteLine(\"reacquired\");"),
        std::string("dispose c\nreacquired\ndispose b\ndispose b2\n"));
}

// --- The move is a RUNTIME fact, so a conditional move is exact -------------

BEZEL_TEST(codegen_choice_local_to_local_move_4566,
           spec_15_3_conditional_move_disposes_the_payload_exactly_once) {
    // The taken path leaves `b` moved-from and `c` owning the payload; the
    // untaken path leaves `b` owning it. Exactly one disposal either way — a
    // suppression keyed on "this local is moved somewhere in the body" would
    // leak on the untaken path, and no suppression at all double-frees on the
    // taken one. Pre-fix this died by signal (MEASURED: `ran == false`) in the
    // `flag=true` leg, which runs first, so the `flag=false` leg never ran.
    expectRunOutput(
        run(kNamedResourceDecls,
            "Host.M(true); Console.WriteLine(\"--\"); Host.M(false);",
            "public static method M(flag: bool) "
            "  var b := Bundle.Ready(new Resource(\"b\")); "
            "  var c := Bundle.Empty; "
            "  if flag then c := b; end; "
            "  Console.WriteLine(\"after\"); "
            "end M;"),
        std::string("after\ndispose b\n--\nafter\ndispose b\n"));
}

// --- §35.9's `C?` spelling takes the same move ------------------------------

BEZEL_TEST(codegen_choice_local_to_local_move_4566,
           spec_35_9_local_to_local_move_of_an_optional_owning_choice) {
    // §35.9: "For an **owning** `C`, the wrapper `C?` is itself move-only."
    // Its storage is `{ i8 hasValue, <choice struct> }` (#4552's layout fix),
    // so the moved-from mark has to be written at the WRAPPER's layout, not at
    // the bare choice struct's — a mark that skipped the wrapper would leave
    // `hasValue` set and the scope-end walk would dispose the moved-out
    // payload a second time. Pre-fix this died by signal (MEASURED:
    // `ran == false`), both slots having dispatched on the one widget.
    expectRunOutput(
        run("class Widget "
            "  private _name: string; "
            "  public constructor(name: string) _name := name; "
            "  end constructor; "
            "  destructor Console.WriteLine($\"dispose {_name}\"); "
            "  end destructor; "
            "end Widget; "
            "choice Outcome Fetched(owned w: Widget), Failed; end Outcome; ",
            "var a: Outcome? := Outcome.Fetched(new Widget(\"a\")); "
            "var b: Outcome? := Outcome.Fetched(new Widget(\"b\")); "
            "b := a; "
            "Console.WriteLine(\"opt moved\");"),
        std::string("dispose b\nopt moved\ndispose a\n"));
}

// --- The loop-amplified count -----------------------------------------------

BEZEL_TEST(codegen_choice_local_to_local_move_4566,
           spec_15_3_every_moved_payload_in_a_loop_is_disposed_exactly_once) {
    // 500 iterations, one `Resource` each, each moved out of the loop-body
    // local `b` into the enclosing `c`. §35.4's drop-on-overwrite disposes the
    // 499 values `c` displaces and the scope-end active-case pass disposes the
    // 500th, so the spec's count is exactly 500 — one per construction. A
    // count ABOVE 500 is the double free this issue is about; a count BELOW it
    // is a suppression that has grown into a leak.
    expectRunOutput(
        run(kCountingResourceDecls,
            // The churn lives in its own method so `c`'s scope-end disposal
            // has already run by the time the count prints.
            "Host.Churn(); Console.WriteLine(Resource._n);",
            "public static method Churn() "
            "  var c := Bundle.Empty; "
            "  var i := 0; "
            "  while i < 500 do "
            "    var b := Bundle.Ready(new Resource()); "
            "    c := b; "
            "    i := i + 1; "
            "  end while; "
            "end Churn;"),
        std::string("500\n"));
}
