// Regression tests for issue #7903 — an `await for` over a source that is NOT
// async-iterable must be REJECTED IN SEMA (§30.7 / §29.13), never lowered
// into malformed IR.
//
// THE DEFECT.  `await for var x in move h.A with token do end;` over an
// inline `[2 owned]Res?` array passed sema (the async path had NO source
// check — the synchronous `for`'s §21.4 check excludes `await for` by
// name) and then failed the LLVM module verifier:
//
//   Load operand must be a pointer.
//     %18 = load ptr, [2 x ptr] %14, align 8
//   Call parameter type does not match function signature!
//   %14 = load [2 x ptr], ptr %13, align 8
//   ptr  %afor.getenum = call ptr %20([2 x ptr] %14, ptr %.reload25)
//   Both operands to ICmp instruction are not of the same type!
//     %afor.selfen = icmp eq ptr %afor.getenum, [2 x ptr] %14
//   Invalid operands for select instruction!
//
// The compiler emitted MALFORMED IR for a program it had accepted — a
// compiler that emits malformed IR is a soundness problem, and a program that
// reaches codegen at all should have been rejected in sema.
//
// WHAT THE DOCUMENTS REQUIRE.  §29.13 (v0.355): "`await for` iterates
// `IAsyncEnumerable<T>` (borrowing) and `IOwningAsyncEnumerable<T>` (owning)
// sources." §30.7 (v0.355) gives the protocol those two heads carry. §15.1.2
// (v0.355): "inline arrays are not class types and do not implement
// interfaces" — an inline array has no `GetAsyncEnumerator`, so it is not an
// async iterable.
//
// THE ORACLE.  `runAot` — the strict AOT oracle (fork-isolated
// `Driver::compileFile` + clang link + native exec).  The pre-fix compiler
// REJECTED this program too, but with the verifier's complaint, not with a
// diagnostic: so the rows below assert the §30.7 TEXT and the ABSENCE of the
// malformed-IR text.  That is what makes them fail on the pre-fix tree.
//
// The MUST-STAY-GREEN controls (a real `IAsyncEnumerable<T>` source, a real
// `IOwningAsyncEnumerable<T>` source, an async-iterator method's result, and
// the SYNCHRONOUS `for` over an inline array) still compile and run.
//
// per §30.7 / §29.13 (v0.355), §15.1.2, §21.4 / §29, issue #7903.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The issue's own repro, with an entry point (the repro as filed had none).
const char* kIssueRepro =
    "module M; "
    "public class Res "
    "  public Id: int; "
    "  public constructor(id: int := 0) this.Id := id; end constructor; "
    "end Res; "
    "public class Holder "
    "  public owned F: Res := new Res(1); "
    "  public owned N: Res? := new Res(2); "
    "  public A: [2 owned]Res? := [new Res(3), null]; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class User "
    "  public static async method Run(h: Holder, token: CancellationToken): Task "
    "    await for var x in move h.A with token do end; "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    using var h := new Holder(); "
    "    await Run(h, CancellationToken.None); "
    "    return 0; "
    "  end Main; "
    "end User; ";

// The compiler MUST reject the program, and the rejection must be the §30.7
// SEMANTIC diagnostic — not the LLVM verifier's complaint about malformed IR.
bool rejectsAsNotAsyncIterable(const std::string& program) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("30.7") != std::string::npos &&
            d.message.find("async-iterable") != std::string::npos)
            sawRule = true;
        // The pre-fix verdict: the module verifier rejected the malformed IR
        // the compiler had emitted. That must NOT be the verdict any more.
        if (d.message.find("malformed IR") != std::string::npos ||
            d.message.find("IR verification failed") != std::string::npos ||
            d.message.find("Load operand must be a pointer") != std::string::npos)
            return false;
    }
    return sawRule;
}

int runExit(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Rejects in sema (§30.7 / §29.13) ─────────────────────────────────────────

// THE ISSUE'S REPRO: `await for` over an inline owning array. Pre-fix this
// reached codegen and produced malformed IR; post-fix it is a §30.7 error.
BEZEL_TEST(await_for_non_async_source_7903, inline_array_rejected_spec_30_7) {
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(kIssueRepro));
}

// §29.13: a SYNCHRONOUS `IEnumerable<T>` source is not async-iterable.
BEZEL_TEST(await_for_non_async_source_7903, sync_enumerable_rejected_spec_30_7) {
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(
        "module M; "
        "class C "
        "  public static async method Consume(src: IEnumerable<int>): Task<int> "
        "    var sum := 0; "
        "    await for var x in src do sum := sum + x; end; "
        "    return sum; "
        "  end Consume; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    return await Consume(xs); "
        "  end Main; "
        "end C; "));
}

// §29.13: a builtin `Array<T>` source implements `IEnumerable<T>`, not
// `IAsyncEnumerable<T>` — the same defect at a second shape.
BEZEL_TEST(await_for_non_async_source_7903, array_rejected_spec_30_7) {
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var sum := 0; "
        "    await for var x in xs do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C; "));
}

// §16.3 / §29.13: a `Range<T>` value is sync-iterable only. Pre-fix this
// reached codegen as a struct value and produced malformed IR.
BEZEL_TEST(await_for_non_async_source_7903, range_rejected_spec_30_7) {
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in 0..3 do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C; "));
}

// §29.13: a record value carries no `GetAsyncEnumerator`.
BEZEL_TEST(await_for_non_async_source_7903, record_rejected_spec_30_7) {
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(
        "module M; "
        "record Pt public X: int; public Y: int; end Pt; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var p := new Pt(1, 2); "
        "    await for var x in p do end; "
        "    return 0; "
        "  end Main; "
        "end C; "));
}

// §30.7 / §29.13: a GENERIC body's type-parameter source is a shape sema
// cannot classify (§14.2's dispatched-body rule leaves the substitution to the
// instantiation), so the CODEGEN guard refuses to lower it — a diagnostic,
// never malformed IR. Pre-fix this reached codegen and produced malformed IR.
BEZEL_TEST(await_for_non_async_source_7903, type_parameter_source_rejected) {
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(
        "module M; "
        "class C "
        "  public static async method Consume<T>(t: T): Task<int> "
        "    await for var x in t do end; "
        "    return 0; "
        "  end Consume; "
        "  public static async method Main(): Task<int> "
        "    return 0; "
        "  end Main; "
        "end C; "));
}

// ── Controls: the async path keeps working (MUST-STAY-GREEN) ─────────────────

// §29.13 / §30.7: an `IAsyncEnumerable<int>` source compiles and iterates.
BEZEL_TEST(await_for_non_async_source_7903, async_enumerable_still_runs) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C; "),
        6);
}

// §29.13 / §30.7: an `IOwningAsyncEnumerable<T>` source compiles and
// iterates.
BEZEL_TEST(await_for_non_async_source_7903, owning_async_enumerable_still_runs) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class Res public Id: int; "
        "  public constructor(id: int := 0) this.Id := id; end constructor; "
        "end Res; "
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<Res> "
        "    yield return new Res(4); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var n := 0; "
        "    await for var r in Gen() do using var x := move r; n := n + x.Id; end; "
        "    return n; "
        "  end Main; "
        "end C; "),
        4);
}

// §21.4 / §15.1.2 / §29.2: the SYNCHRONOUS `for` over an inline array is
// unchanged — an inline array IS sync-iterable (the direct indexed loop), so
// the async check must not touch it.
BEZEL_TEST(await_for_non_async_source_7903, sync_for_over_inline_array_still_runs) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class Res public Id: int; "
        "  public constructor(id: int := 0) this.Id := id; end constructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var a: [2 owned]Res? := [new Res(3), null]; "
        "    var n := 0; "
        "    for var x in a do n := n + 1; end; "
        "    return n; "
        "  end Main; "
        "end C; "),
        2);
}
