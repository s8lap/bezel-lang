// Spec-derived tests for issue #7903 — an `await for` over a source that is
// NOT async-iterable must be rejected in SEMA with a §30.7 / §29.13
// diagnostic.
//
// Root cause: the `await for` header had NO source check at all. The
// SYNCHRONOUS `for` has one — ExprTC::visitStmt's §21.4 / §29 ITERABLE-source
// check (issue #1775) — and its own comment says the async path is excluded
// ("`await for` (§30.7) iterates an async source through a separate path, so
// it is excluded here"). That exclusion was the HOLE: the async path had no
// equivalent check, so a source that is not async-iterable passed sema and
// reached codegen, where `lowerAwaitFor` read the source's vtable out of a
// value that is not a pointer and emitted MALFORMED IR — issue #7903's repro,
// `await for var x in move h.A with token do end;` over an inline
// `[2 owned]Res?` array, produced "Load operand must be a pointer.
// %18 = load ptr, [2 x ptr] %14".
//
// WHAT THE DOCUMENTS REQUIRE.  §29.13 (v0.355) states the rule verbatim:
// "`await for` iterates `IAsyncEnumerable<T>` (borrowing) and
// `IOwningAsyncEnumerable<T>` (owning) sources." §30.7 (v0.355) gives the
// protocol those two heads carry (`GetAsyncEnumerator(token)` on the source;
// `MoveNextAsync()` / `Current` / `DisposeAsync` on the enumerator). An
// inline array is none of these: §15.1.2 (v0.355) says "inline arrays are not
// class types and do not implement interfaces", so it has no
// `GetAsyncEnumerator` at all — it is §29.2's DIRECT-INDEXED source, and only
// for the SYNCHRONOUS `for`.
//
// The fix adds the async twin of the §21.4 check to ExprTC::visitStmt
// (ExprTypecheck.cpp): resolve the `await for` source's type and, when it is
// definitely not async-iterable, emit the §30.7 / §29.13 error. The
// MUST-STAY-GREEN controls (a real `IAsyncEnumerable<T>` /
// `IOwningAsyncEnumerable<T>` source, an async-iterator method's result, and
// the synchronous `for` over an inline array) are unaffected.
//
// per §30.7 / §29.13 (v0.355), §15.1.2, §21.4 / §29, issue #7903.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

// The §30.7 / §29.13 diagnostic's own text. A reject that fired for another
// reason would not carry it.
bool rejectsAsNotAsyncIterable(const bezel::test::SemaResult& r) {
    return r.diagnostics.hasErrors() &&
           anyDiagContains(r, "30.7") && anyDiagContains(r, "async-iterable");
}

// An async method with an `await for` over the given source expression.
std::string awaitForOver(const std::string& source) {
    return std::string("module M; class C "
                       "  public static async method Main(): Task<int> "
                       "    await for var x in ") +
           source + " do end; "
                      "    return 0; "
                      "  end Main; "
                      "end C;";
}

// A class implementing the §29 `IEnumerable<int>` protocol — a SYNCHRONOUS
// iterable, never an async one.
constexpr const char* kSyncEnumerable =
    "class IntEnum: IEnumerator<int> "
    "  public method MoveNext(): bool return false; end MoveNext; "
    "  public property Current: int get => 0; end Current; "
    "end IntEnum; "
    "class IntRange: IEnumerable<int> "
    "  public method GetEnumerator(): IEnumerator<int> return new IntEnum(); end GetEnumerator; "
    "end IntRange; ";

// A class implementing the §30.7 `IAsyncEnumerable<int>` protocol — an async
// iterable.
constexpr const char* kAsyncEnumerable =
    "class AsyncSeq: IAsyncEnumerable<int> "
    "  public method GetAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IAsyncEnumerator<int> "
    "    return new AsyncEnum(); "
    "  end GetAsyncEnumerator; "
    "end AsyncSeq; "
    "class AsyncEnum: IAsyncEnumerator<int> "
    "  public method MoveNextAsync(): Task<bool> return Task.FromResult(false); end MoveNextAsync; "
    "  public property Current: int get => 0; end Current; "
    "end AsyncEnum; ";

}  // namespace

// ── Rejects: a source that is not async-iterable (§30.7 / §29.13) ────────────

// §15.1.2 / §29.13: an inline array is not a class type and implements no
// interface, so it is not an async iterable. This is the issue's own repro
// shape (an owning, nullable inline array) — the shape that reached codegen
// and produced malformed IR.
BEZEL_TEST(sema_await_for_source_7903, inline_array_source_rejected_spec_30_7) {
    auto r = parseSemaAll(
        "module M; class Res public Id: int := 0; end Res; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var a: [2 owned]Res? := [new Res(3), null]; "
        "    await for var x in a do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// §29.13: a SYNCHRONOUS `IEnumerable<T>` source is not async-iterable — the
// clearest form of the "not async-iterable" case.
BEZEL_TEST(sema_await_for_source_7903, sync_enumerable_source_rejected_spec_30_7) {
    auto r = parseSemaAll(
        std::string("module M; ") + kSyncEnumerable +
        "class C "
        "  public static async method Main(r: IntRange): Task<int> "
        "    await for var x in r do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// §29.13: a builtin collection (`Array<T>`) implements `IEnumerable<T>`, not
// `IAsyncEnumerable<T>` — the SAME defect at a second shape.
BEZEL_TEST(sema_await_for_source_7903, array_source_rejected_spec_30_7) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    await for var x in xs do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// §16.3 / §29.13: a `Range<T>` is a sync-iterable value, never an async
// iterable.
BEZEL_TEST(sema_await_for_source_7903, range_source_rejected_spec_30_7) {
    auto r = parseSemaAll(awaitForOver("0..3"));
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// §29.13: a record is a value type with no `GetAsyncEnumerator`.
BEZEL_TEST(sema_await_for_source_7903, record_source_rejected_spec_30_7) {
    auto r = parseSemaAll(
        "module M; record Pt public X: int; public Y: int; end Pt; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var p := new Pt(1, 2); "
        "    await for var x in p do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// §29.13: a primitive is not an object carrying the §30.7 protocol.
BEZEL_TEST(sema_await_for_source_7903, int_source_rejected_spec_30_7) {
    auto r = parseSemaAll(awaitForOver("3"));
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// §4.6 / §29.13: a string iterates its chars over the SYNCHRONOUS protocol.
BEZEL_TEST(sema_await_for_source_7903, string_source_rejected_spec_30_7) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var s := \"abc\"; "
        "    await for var x in s do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectsAsNotAsyncIterable(r));
}

// ── Accepts: the async path keeps working (MUST-STAY-GREEN) ─────────────────

// §29.13: an `IAsyncEnumerable<T>`-typed source is a real async source.
BEZEL_TEST(sema_await_for_source_7903, async_enumerable_source_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Main(src: IAsyncEnumerable<int>): Task<int> "
        "    await for var x in src do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "async-iterable"));
}

// §29.13 / §30.7: an `IOwningAsyncEnumerable<T>`-typed source is a real async
// source (the owning half of the pair).
BEZEL_TEST(sema_await_for_source_7903, owning_async_enumerable_source_accepted) {
    auto r = parseSemaAll(
        "module M; class Res public Id: int := 0; end Res; "
        "class C "
        "  public static async method Main(src: IOwningAsyncEnumerable<Res>): Task<int> "
        "    await for var x in src do using var r := move x; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "async-iterable"));
}

// §29.13: a USER class implementing `IAsyncEnumerable<int>` is an async
// source — the base-closure walk must find the protocol through the
// interface base.
BEZEL_TEST(sema_await_for_source_7903, user_async_enumerable_source_accepted) {
    auto r = parseSemaAll(
        std::string("module M; ") + kAsyncEnumerable +
        "class C "
        "  public static async method Main(s: AsyncSeq): Task<int> "
        "    await for var x in s do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "async-iterable"));
}

// §29.3 / §30.7: an async-iterator method's result is an
// `IAsyncEnumerable<T>` — the canonical `await for` source.
BEZEL_TEST(sema_await_for_source_7903, async_iterator_result_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await for var x in Gen() do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "async-iterable"));
}

// §21.4 / §29.2: the SYNCHRONOUS `for` over an inline array is unchanged —
// an inline array IS sync-iterable (§15.1.2's direct indexed loop), so the
// async check must not touch it.
BEZEL_TEST(sema_await_for_source_7903, sync_for_over_inline_array_unchanged) {
    auto r = parseSemaAll(
        "module M; class Res public Id: int := 0; end Res; "
        "class C "
        "  public static method Main(): int "
        "    var a: [2 owned]Res? := [new Res(3), null]; "
        "    var n := 0; "
        "    for var x in a do n := n + 1; end; "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "async-iterable"));
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "21.4"));
}

// §21.4 (#1775): the SYNCHRONOUS `for`'s own non-iterable check is
// unchanged — a bare `int` source is still rejected by §21.4, and the async
// check does not fire on it (it is not an `await for`).
BEZEL_TEST(sema_await_for_source_7903, sync_for_over_int_still_spec_21_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(n: int): int "
        "    for var x in n do end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "21.4"));
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "async-iterable"));
}
