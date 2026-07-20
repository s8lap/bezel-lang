// Codegen tests for §35 (v0.200, #2045): choice types — the runtime
// representation, construction, and match lowering for value-like choices.
// These exercise end-to-end AOT compilation+execution: a choice value is
// constructed (`Opt.None` payload-free, `Opt.Some(42)` payload-carrying),
// matched (`Some(var v) => …, None => …`), and the dispatched arm's
// observable behavior verified via exit code or stdout.
//
// What this pins:
//   * §35.13 layout: the choice struct `{ i32 tag, case0_payload, … }`
//     produced by Emitter::choiceStruct.
//   * §35.3 construction: payload-free case materializes via
//     Emitter::choiceCaseConstant; payload case via Emitter::lowerCall's
//     ChoiceCaseSymbol branch (tag store + payload slot fill).
//   * §35.5 match: Emitter::patternMatches' choice branch — tag read +
//     ICmpEQ + per-slot payload binding (naked, `var x`, `_`).
//   * §19.4 stmt + §19.4 expr position both work.
//
// §35.4 classification is implicit: every test here uses a VALUE-LIKE
// choice (no `owned`/class-typed payloads), so the classifier (slice 1)
// returns ValueLike and no sendable/disposal rule rejects the program.
// Owning/borrow-bearing choices + the owning match (§35.5 owning) land in
// follow-up slices — the codegen here is the foundation for value-like
// choices that actually RUN.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Wrap a method body in the standard module+class shell and run it; return
// the exit code (-999 if the run failed). Mirrors case_stmt_test.cpp.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Same as `run` but allow extra top-level decls (a choice declaration must
// precede the class). Used for tests that need a custom choice shape.
int runDecls(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Run a program and return its stdout (for tests that observe output
// rather than exit code — e.g. a string-payload match that prints).
std::string runOut(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.stdoutText : std::string();
}

}  // namespace

// --- §35.3: payload-free case construction + match -------------------------

BEZEL_TEST(codegen_choice_35, spec_35_3_payload_free_first_case_matches) {
    // §35.3: `Color.Red` constructs a value with Red active; §35.5: the
    // matching `Red` arm fires. Per §35.13 layout, the tag is Red's
    // declaration-order index (0) — first case.
    auto r = runDecls(
        "choice Color Red, Green, Blue; end Color;",
        "var x: Color := Color.Red; "
        "case x of "
        "  Red   => do return 10; end, "
        "  Green => do return 20; end, "
        "  Blue  => do return 30; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 10);
}

BEZEL_TEST(codegen_choice_35, spec_35_3_payload_free_middle_case_matches) {
    // §35.13 tag = declaration-order index. Green is index 1.
    auto r = runDecls(
        "choice Color Red, Green, Blue; end Color;",
        "var x: Color := Color.Green; "
        "case x of "
        "  Red   => do return 10; end, "
        "  Green => do return 20; end, "
        "  Blue  => do return 30; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 20);
}

BEZEL_TEST(codegen_choice_35, spec_35_3_payload_free_last_case_matches) {
    // Blue is index 2.
    auto r = runDecls(
        "choice Color Red, Green, Blue; end Color;",
        "var x: Color := Color.Blue; "
        "case x of "
        "  Red   => do return 10; end, "
        "  Green => do return 20; end, "
        "  Blue  => do return 30; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 30);
}

// --- §35.3: single-payload case construction + match -----------------------

BEZEL_TEST(codegen_choice_35,
           spec_35_3_single_payload_some_arm_binds_value) {
    // §35.3 call form `Opt.Some(42)` + §35.5 destructuring `Some(var v)`.
    // The payload value flows from construction through the slot to the
    // arm-local binding `v`.
    auto r = runDecls(
        "choice Opt Some(value: int), None; end Opt;",
        "var x: Opt := Opt.Some(42); "
        "case x of "
        "  Some(var v) => do return v; end, "
        "  None        => do return 0; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 42);
}

BEZEL_TEST(codegen_choice_35,
           spec_35_3_single_payload_none_arm_matches_none_value) {
    // §35.3 payload-free construction + §35.5 payload-free pattern match.
    auto r = runDecls(
        "choice Opt Some(value: int), None; end Opt;",
        "var x: Opt := Opt.None; "
        "case x of "
        "  Some(var v) => do return v; end, "
        "  None        => do return 99; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 99);
}

// --- §35.3: multi-payload case ---------------------------------------------

BEZEL_TEST(codegen_choice_35,
           spec_35_3_multi_payload_first_arm_binds_both_fields) {
    // §35.5 "a positional destructuring pattern `CaseName(slot, …)` with
    // the case's exact payload arity." A 2-field payload binds both.
    auto r = runDecls(
        "choice Pair First(a: int, b: int), Zero; end Pair;",
        "var x: Pair := Pair.First(7, 35); "
        "case x of "
        "  First(var a, var b) => do return a + b; end, "
        "  Zero                 => do return 0; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 42);  // 7 + 35
}

BEZEL_TEST(codegen_choice_35,
           spec_35_5_multi_payload_naked_binding_slot) {
    // §19.3 slot rules carry over (§35.5): a naked identifier slot binds
    // just like `var x`. `First(a, b)` — bare names.
    auto r = runDecls(
        "choice Pair First(a: int, b: int), Zero; end Pair;",
        "var x: Pair := Pair.First(15, 4); "
        "case x of "
        "  First(a, b) => do return a * b; end, "
        "  Zero         => do return 0; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 60);  // 15 * 4
}

// --- §35.5: discard slot ---------------------------------------------------

BEZEL_TEST(codegen_choice_35, spec_35_5_discard_slot) {
    // §35.5 / §19.3: per-slot `_` discards. `Some(_)` discards the
    // payload — the explicit form for "matched, payload ignored."
    auto r = runDecls(
        "choice Opt Some(value: int), None; end Opt;",
        "var x: Opt := Opt.Some(123); "
        "case x of "
        "  Some(_) => do return 1; end, "
        "  None    => do return 0; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 1);
}

// --- §35.5: case-expr (expression position) --------------------------------

BEZEL_TEST(codegen_choice_35,
           spec_35_5_case_expr_position_returns_arm_value) {
    // §19.4 / §35.5: a `case` in expression position yields the selected
    // arm's value. The scrutinee temporary is the choice struct value;
    // the matched arm's expression returns the bound payload.
    auto r = runDecls(
        "choice Opt Some(value: int), None; end Opt;",
        "var x: Opt := Opt.Some(7); "
        "var r: int := case x of "
        "  Some(var v) => v, "
        "  None        => 0 "
        "end; "
        "return r;");
    BEZEL_EXPECT_EQ(r, 7);
}

// --- §35.4 + §35.5: value-like choice with string payload ------------------

BEZEL_TEST(codegen_choice_35,
           spec_35_4_string_payload_round_trip_through_stdout) {
    // §35.4 final paragraph: string payloads are classification-neutral
    // (runtime-managed reference type). §35.5: the payload binding is a
    // shared reference (no copy). Round-trip through stdout — `s` is the
    // same `bz_string*` that was stored at construction.
    auto out = runOut(
        "choice Log Msg(text: string), Empty; end Log;",
        "var x: Log := Log.Msg(\"hello\"); "
        "case x of "
        "  Msg(var s) => do Console.WriteLine(s); return 1; end, "
        "  Empty      => do return 0; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_TRUE(out.find("hello") != std::string::npos);
}

// --- §35.5: arm-priority (first match wins) over a choice scrutinee --------

BEZEL_TEST(codegen_choice_35, spec_35_5_first_match_wins) {
    // §19.4 (§35.5 carries the rule over): the first arm whose pattern
    // matches wins. Two arms with the same case name — the first fires.
    // (The sema payload-bare rejection does not fire here because both
    // arms use the same destructuring form.)
    auto r = runDecls(
        "choice Opt Some(value: int), None; end Opt;",
        "var x: Opt := Opt.Some(5); "
        "case x of "
        "  Some(var v) => do return 100; end, "
        "  Some(var v) => do return 200; end, "
        "  None        => do return 0; end "
        "end; "
        "return 0;");
    BEZEL_EXPECT_EQ(r, 100);
}
