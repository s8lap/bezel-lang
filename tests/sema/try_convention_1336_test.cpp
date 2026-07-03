// v0.171 (#1336) — the Try-convention revision (sema matrix). Every extraction
// Try's out parameter is nullable (`T?`/`V?`, stdlib §1 / §4): the false path
// assigns `null` (§22.3). The call-site consequences the type checker enforces:
//
//   (a) an explicit plain `out x` recipient with a NON-nullable declared type is
//       REJECTED (nullability mismatch) — it cannot receive the false-path null;
//   (b) an explicit `out x` with a nullable (`T?`) recipient is ACCEPTED;
//   (c) an inline `out var x` infers `T?` — a direct non-nullable use of `x`
//       (field access, arithmetic, non-nullable return) is REJECTED without `!`;
//   (d) inline `out owned var` / `out using var` (owning families) infer `T?`
//       and their `!`/`?.`-narrowed discharge is ACCEPTED;
//   (e) `out _` (discard) is ACCEPTED.
//
// // per stdlib §1/§4.5/§4.8/§4.9/§4.11, §22.3, #1336

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {
std::string withMain(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  do ") + body + " end; return 0; end Main; end C;";
}
bool mentionsNullableOut(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("must be nullable") != std::string::npos &&
            d.message.find("#1336") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// (a) explicit NON-nullable recipient — Stack<int>.TryPop → REJECT.
BEZEL_TEST(try_convention_1336_sema, explicit_nonnullable_stack_int_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new Stack<int>(); s.Push(1); "
        "var x: int := 0; var ok := s.TryPop(out x); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsNullableOut(r));
}

// (a) explicit NON-nullable recipient — Dictionary.TryGetValue → REJECT.
BEZEL_TEST(try_convention_1336_sema, explicit_nonnullable_dict_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var d := new Dictionary<string, int>(); d.Add(\"a\", 1); "
        "var v: int := 0; var ok := d.TryGetValue(\"a\", out v); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsNullableOut(r));
}

// (b) explicit NULLABLE recipient — ACCEPT.
BEZEL_TEST(try_convention_1336_sema, explicit_nullable_recipient_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new Stack<int>(); s.Push(1); "
        "var x: int? := null; var ok := s.TryPop(out x); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) inline `out var x` infers `int?` — a non-nullable use without `!` REJECTS.
BEZEL_TEST(try_convention_1336_sema, inline_out_var_infers_nullable_direct_use_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new Stack<int>(); s.Push(1); "
        "if s.TryPop(out var x) then return x; end; "));  // return int? as int
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (c') the same inline form with `!` is ACCEPTED.
BEZEL_TEST(try_convention_1336_sema, inline_out_var_with_bang_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new Stack<int>(); s.Push(1); "
        "if s.TryPop(out var x) then return x!; end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (d) inline `out owned var` (OwningStack) infers `Cell?`; `!`-narrowed use +
//     `?.`-discharge is ACCEPTED.
BEZEL_TEST(try_convention_1336_sema, inline_out_owned_var_bang_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwningStack<Cell>(); s.Push(move (new Cell(1))); "
        "if s.TryPop(out owned var item) then "
        "  var v := item!._v; item!.Dispose(); return v; end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (d') inline `out owned var` — a DIRECT non-nullable access without `!` REJECTS.
BEZEL_TEST(try_convention_1336_sema, inline_out_owned_var_direct_access_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwningStack<Cell>(); s.Push(move (new Cell(1))); "
        "if s.TryPop(out owned var item) then "
        "  var v := item._v; item.Dispose(); return v; end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (d'') inline `out using var` (OwningQueue.TryDequeue) infers `Cell?`; the
//       `!`-narrowed field read is ACCEPTED (the using-var discharges the value).
BEZEL_TEST(try_convention_1336_sema, inline_out_using_var_bang_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var q := new OwningQueue<Cell>(); q.Enqueue(move (new Cell(2))); "
        "var got := -1; if q.TryDequeue(out using var item) then "
        "  got := item!._v; end; return got; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (e) `out _` discard — ACCEPT (no recipient to type-check).
BEZEL_TEST(try_convention_1336_sema, discard_recipient_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var d := new Dictionary<string, int>(); d.Add(\"a\", 1); "
        "if d.TryGetValue(\"a\", out _) then return 1; end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
