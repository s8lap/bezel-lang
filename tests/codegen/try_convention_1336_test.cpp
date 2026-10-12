// v0.171 (#1336) — the Try-convention revision. Every extraction Try's out
// parameter is nullable (`T?`/`V?`): the FALSE path assigns `null` (§22.3 —
// every out assigned on every exit path), the TRUE path the extracted value.
// For value-typed instantiations the out slot is the tagged `{i8 hasValue, T}`
// struct, so the caller reads a `T?` (stdlib §1 / §4.8 / §4.11).
//
// These are the codegen value tests per family: the TRUE-path value and the
// FALSE-path observable `null`, including a value-typed `T?` case
// (Stack<int>.TryPop) that the pre-v0.171 non-nullable shape could not express.
//
// Value tests — runJit returns the raw int (kept in 0..255).
//
// // per stdlib §1/§4.8/§4.11, §22.3, #1336

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── Borrowing value instantiations — the `T?` tagged-struct out slot ──────────

// §4.11 (#1336): Stack<int>.TryPop — TRUE path yields the value, read via `!`.
BEZEL_TEST(try_convention_1336, stack_int_trypop_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var s := new Stack<int>(); s.Push(42); "
        "    if s.TryPop(out var x) then return x!; end; return 0; end; return 0; "
        "end Main; end C;"), 42);
}

// §4.11 / §22.3 (#1336): Stack<int>.TryPop on an EMPTY stack — the FALSE path
// assigns `null` to the `int?` out slot (observably null, not a 0 sentinel).
BEZEL_TEST(try_convention_1336, stack_int_trypop_false_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var s := new Stack<int>(); "
        "    var ok := s.TryPop(out var x); "
        "    if x = null then return 7; end; return 3; end; return 0; "
        "end Main; end C;"), 7);
}

// §4.11 (#1336): Queue<int>.TryDequeue — TRUE and FALSE paths.
BEZEL_TEST(try_convention_1336, queue_int_trydequeue_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var q := new Queue<int>(); q.Enqueue(15); "
        "    if q.TryDequeue(out var x) then return x!; end; return 0; end; return 0; "
        "end Main; end C;"), 15);
}
BEZEL_TEST(try_convention_1336, queue_int_trydequeue_false_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var q := new Queue<int>(); var v: int? := 9; "
        "    var ok := q.TryDequeue(out v); "
        "    if v = null then return 7; end; return 3; end; return 0; "
        "end Main; end C;"), 7);
}

// §4.11 (#1336): Stack<int>.TryPeek — borrow form, value out `T?`, no removal.
BEZEL_TEST(try_convention_1336, stack_int_trypeek_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var s := new Stack<int>(); s.Push(5); "
        "    if s.TryPeek(out var p) then return p!; end; return 0; end; return 0; "
        "end Main; end C;"), 5);
}

// §4.8 (#1336): Dictionary<string,int>.TryGetValue — TRUE path value via `!`.
BEZEL_TEST(try_convention_1336, dict_int_trygetvalue_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var d := new Dictionary<string,int>(); d.Add(\"a\", 33); "
        "    if d.TryGetValue(\"a\", out var v) then return v!; end; return 0; end; return 0; "
        "end Main; end C;"), 33);
}
// §4.8 / §22.3 (#1336): missing key — FALSE path assigns `null` to `int?`.
BEZEL_TEST(try_convention_1336, dict_int_trygetvalue_false_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var d := new Dictionary<string,int>(); var v: int? := 5; "
        "    var ok := d.TryGetValue(\"zz\", out v); "
        "    if v = null then return 7; end; return 3; end; return 0; "
        "end Main; end C;"), 7);
}

// §4.11 (#1336): Queue<int>.TryDequeue empty — inline `out var`, false → null.
BEZEL_TEST(try_convention_1336, queue_int_trydequeue_inline_empty_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do var q := new Queue<int>(); "
        "    var ok := q.TryDequeue(out var x); "
        "    if x = null then return 7; end; return 3; end; return 0; "
        "end Main; end C;"), 7);
}

// ── Owning class instantiations — nullable-pointer out slot, `!`/`?.` idiom ───

// §4.5 (#1336): OwningArray<Box>.TryPopFirst — TRUE path, `!` to read/dispose.
BEZEL_TEST(try_convention_1336, owning_array_trypopfirst_true_bang) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); a.Add(move (new Box(10))); "
        "    if a.TryPopFirst(out var item) then "
        "      var r := item!.Val; item!.Dispose(); return r; end; return 0; end; return 0; "
        "end Main; end C;"), 10);
}

// §4.5 / §22.3 (#1336): empty array — FALSE path leaves the inline `out owned
// var` recipient `null`; the null-guarded `?.Dispose()` is a safe no-op.
BEZEL_TEST(try_convention_1336, owning_array_trypopfirst_false_is_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box public Val: int := 0; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    var ok := a.TryPopFirst(out var item); "
        "    item?.Dispose(); "
        "    if item = null then return 7; end; return 3; end; return 0; "
        "end Main; end C;"), 7);
}

// §4.9 (#1336): OwningDictionary<string,R>.TryPopValue — owned `V?` out; the
// popped value is transferred out and disposed exactly once (live count back to
// 0). R._n is a live-object counter: constructor +1, destructor -1.
BEZEL_TEST(try_convention_1336, owning_dict_trypopvalue_true_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  public constructor() R._n := R._n + 1; end constructor; "
        "  destructor R._n := R._n - 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, R>(); "
        "    d.Add(\"a\", move (new R())); "                          // _n = 1
        "    if d.TryPopValue(\"a\", out var v) then v?.Dispose(); end; "  // → _n = 0
        "  end; return R._n; "        // 0 iff popped value disposed exactly once
        "end Main; end C;"), 0);
}
