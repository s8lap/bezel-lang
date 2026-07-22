// Behavior tests for keyword-led lambdas and the return-type ascription —
// spec §27.3 / §27.4 / §30.6, v0.217 (proposal 0064 graduation; impl #2101).
//
// The v0.217 cutover respelled lambdas `lambda(params): T? => body` and added
// the one capability the old spelling could not host: an optional RETURN-TYPE
// ASCRIPTION behaving exactly as a method's declared return type —
//   - §5.1 implicit widening at each `return` / at the expression body,
//   - the v0.159 typed-context literal widening (`lambda(): long =>
//     3_000_000_000`),
//   - a declared nullable return admitting `null`-producing arms (§20) — the
//     §27.3 divergent-return error's THIRD named remedy (the spec's own
//     `classify` example),
//   - a unit ascription (`: ()`) spelling a procedure lambda,
//   - a function-type ascription ending before `=>` (`lambda(n: int):
//     (int) -> int => …`, proposal 0064 §2's adder).
// When the ascription is absent, all §27.3/§27.4 inference is unchanged —
// the target-typed pipeline and destructuring cases below pin that.
//
// Each case FAILS without the #2101 parser+sema change (the pre-v0.217
// compiler lexes `lambda` as an identifier, so `lambda(x: int)` parses as a
// call) and PASSES with it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §27.4 v0.217: target-typed pipeline — parameters and return inferred from
// the LINQ targets, exactly as before the cutover (proposal 0064 §2's
// "dominant case"). 5+8+3 positive, doubled = 32.
BEZEL_TEST(lambda_ascription_2101, spec_27_4_target_typed_pipeline) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var nums: Array<int> := [5, -2, 8, -1, 3]; "
        "var total := nums.Where(lambda(x) => x > 0)"
        ".Select(lambda(x) => x * 2).Sum(); "
        "return total; "
        "end Main; end C;"), 32);
}

// §27.3/§27.4 v0.217: no target type — ascribed params, inferred return
// (unchanged inference), called through the function-typed local.
BEZEL_TEST(lambda_ascription_2101, spec_27_4_no_target_ascribed_params) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var twice := lambda(x: int) => x * 2; "
        "return twice(21); "
        "end Main; end C;"), 42);
}

// §27.3 v0.217 (the spec's own `classify` example): a block body with
// divergent returns (`null` / string literals) is self-contained under a
// `: string?` ascription — the declared nullable return gives the
// null-producing arm its target (§20) and each `return` checks under §5.1.
// This is the divergent-return error's third named remedy, exercised as an
// ACCEPT.
BEZEL_TEST(lambda_ascription_2101, spec_27_3_classify_nullable_ascription) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var classify := lambda(score: int): string? => do "
        "  if score < 0 then return null; end; "
        "  if score >= 90 then return \"A\"; end; "
        "  return \"pass\"; "
        "end; "
        "var a := classify(95); "
        "var b := classify(-1); "
        "if a is null then return 1; end; "
        "if b is not null then return 2; end; "
        "if a = \"A\" then return 0; end; "
        "return 3; "
        "end Main; end C;"), 0);
}

// §27.3 v0.217 / §5.1 v0.159: the typed-context literal widening extends to
// the ascription — an unsuffixed literal exceeding int's range widens to the
// ascribed `long` (the spec's own `lambda(): long => 3_000_000_000`).
BEZEL_TEST(lambda_ascription_2101, spec_27_3_long_literal_widening) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var big := lambda(): long => 3_000_000_000; "
        "Console.WriteLine(big()); "
        "return 0; "
        "end Main; end C;"), std::string("3000000000"));
}

// §27.3 v0.217 (proposal 0064 §2's adder): a FUNCTION-TYPE return ascription
// ends before `=>`; the body is itself a lambda closing over `n`.
BEZEL_TEST(lambda_ascription_2101, spec_27_3_function_type_ascription) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "var adder := lambda(n: int): (int) -> int => lambda(m: int) => n + m; "
        "var add5 := adder(5); "
        "return add5(37); "
        "end Main; end C;"), 42);
}

// §27.3 v0.217: a unit ascription (`: ()`) declares a procedure lambda whose
// single-expression body result is discarded (§27.3's procedure form).
BEZEL_TEST(lambda_ascription_2101, spec_27_3_unit_ascription_procedure) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var log := lambda(msg: string): () => Console.WriteLine(msg); "
        "log(\"hello\"); "
        "return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("hello") != std::string::npos);
}

// §30.6 v0.217: `async lambda(…)` is the modifier order (v0.217 decision 3);
// the async lambda's body awaits and the caller awaits the returned task.
BEZEL_TEST(lambda_ascription_2101, spec_30_6_async_lambda) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static async method Produce(): Task<int> return 21; end Produce; "
        "public static async method Run(): Task<int> "
        "  var f: () -> Task<int> := async lambda() => do "
        "    using var t := Produce(); "
        "    return await t * 2; "
        "  end; "
        "  using var t2 := f(); "
        "  return await t2; "
        "end Run; "
        "public static method Main(): int "
        "  using var t := Run(); "
        "  return t.Result; "
        "end Main; end C;"), 42);
}

// §30.6/§27.3 v0.217 ASCRIBED-async ACCEPT forms (spine review F1: the reject
// side was pinned but no positive `: Task<T>` ascription ever compiled).
// Expression body: the ascription names the Task family; the body types
// against the peeled element.
BEZEL_TEST(lambda_ascription_2101, spec_30_6_async_ascribed_task_expr_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static async method Run(): Task<int> "
        "  var f: () -> Task<int> := async lambda(): Task<int> => 21; "
        "  using var t := f(); "
        "  return await t * 2; "
        "end Run; "
        "public static method Main(): int "
        "  using var t := Run(); "
        "  return t.Result; "
        "end Main; end C;"), 42);
}

// Block (`do`) body with the ascription — the codegen path the review noted
// never existed pre-v0.217 (the old override was expr-body-async only).
BEZEL_TEST(lambda_ascription_2101, spec_30_6_async_ascribed_task_block_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static async method Produce(): Task<int> return 21; end Produce; "
        "public static async method Run(): Task<int> "
        "  var f: () -> Task<int> := async lambda(): Task<int> => do "
        "    using var t := Produce(); "
        "    return await t * 2; "
        "  end; "
        "  using var t2 := f(); "
        "  return await t2; "
        "end Run; "
        "public static method Main(): int "
        "  using var t := Run(); "
        "  return t.Result; "
        "end Main; end C;"), 42);
}

// §27.4 (v0.183, #1408) under the v0.217 spelling: a destructuring head in a
// target-typed position — `lambda(Entry(_, v)) => v`.
BEZEL_TEST(lambda_ascription_2101, spec_27_4_destructuring_head) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Entry(k: string, v: int); "
        "class C public static method Main(): int "
        "var entries: Array<Entry> := [new Entry(\"a\", 20), new Entry(\"b\", 22)]; "
        "var total := entries.Select(lambda(Entry(_, v)) => v).Sum(); "
        "return total; "
        "end Main; end C;"), 42);
}

// §27.3 v0.217 / §5.1: implicit widening at each `return` under a wider
// ascribed integer return — `return x;` (int) under `: long`.
BEZEL_TEST(lambda_ascription_2101, spec_27_3_return_widens_to_ascription) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var up := lambda(x: int): long => do return x; end; "
        "var offset: long := 3_000_000_000; "
        "Console.WriteLine(up(7) + offset); "
        "return 0; "
        "end Main; end C;"), std::string("3000000007"));
}
