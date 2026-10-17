// Codegen — the EAGER-TERMINAL residual of an unbound `owned` receiver.
// §28.3 (unbound owned temporaries), §28.9, stdlib §4.4–§4.11, §5, v0.338.
//
// WHAT #6380 LEFT BEHIND, AND WHY IT IS A DIFFERENT DEFECT. #6380 made
// `lowerToLazyEnumerator` TRANSFER the source it wraps into the `bz_lazy_t*` it
// builds (`bz_lazy_own_source`), so `bz_lazy_dispose` reclaims it. That closes
// the whole `lazyTerminal` path. #6380's own test file declined three shapes and
// named the second explicitly:
//
//   "(2) The eager `Array<T>` terminal arms that never call
//    `freeFreshInputTemp` at all (`.Any()`, `.First()`, `.Max()` on a plain
//    `Array<int>`) leak by a DIFFERENT mechanism — no lazy enumerator is built
//    there at all, so no ownership transfer reaches them."
//
// That is what these rows pin. On an eager receiver NO `bz_lazy_t*` is ever
// built, so #6380 has nothing to act on and the leak is byte-identical before
// and after it.
//
// THE MECHANISM. `Emitter::tryLowerCollectionInstanceCall` materializes the
// receiver into `lazyTempArr` only when `lazyTerminal` (LlvmBackend_Calls.cpp
// :2497) holds — a lazy / LinkedList / Range / vtable-IEnumerable / string / Set
// receiver. An `Array<T>` or `Stack<T>` receiver with NO preceding lazy
// operator keeps `lazyTerminal == false`, so it reaches the terminal DIRECTLY.
// Its two reclaim helpers are then complementary on one flag:
//
//   freeLazyTemp(...)      LlvmBackend_Calls.cpp:2696   `if (!lazyTempArr) return;`
//   freeFreshInputTemp()   LlvmBackend_Calls.cpp:2743   `if (lazyTempArr) return;`
//
// so on that path `freeLazyTemp` is INERT and `freeFreshInputTemp` is the ONLY
// reclaim. Inside the `isArr` block (LlvmBackend_Calls.cpp:2763–7873) five
// terminal returns called `freeFreshInputTemp` — `.Length`/`.Count` (no-arg),
// `.Sum` (the int/long accumulator arm), and the three copy-forms — and the
// other twenty-eight did not. That asymmetry IS the defect: `.Sum` and `.Count`
// were leak-free for no reason a reader could find in the code, and their
// siblings leaked a whole container per evaluation.
//
// MEASURED on this fix's parent (staging `r30-c1` @ a3065a15), 101-iteration
// loops under ASan, receiver an `owned`-returning USER method consumed only in
// receiver position (`f.Make().Terminal()`). A 3x3x16 matrix (3 families x 3
// operator positions x 16 terminals) plus a 3x3x17 single-element matrix, plus
// IndexOf and a decimal `Sum`:
//
//   receiver family  operator pos 0   operator pos 1/2
//   Array<T>         30 leaking       0
//   Stack<T>         14 leaking       0
//   LinkedList<T>    1  (Contains)    0
//
// 12,120 B per shape (12,000 when the terminal's own inline lambda closure is
// released too), i.e. one whole container per evaluation, against 0 for the
// byte-identical `_ := f.Make();` binding — the asymmetry that identifies it.
// The 14 terminals are Any, All, Contains, First, Single, Last, Min, Max,
// Average, Aggregate, ElementAt, FirstOrNull, LastOrNull, SingleOrNull, plus
// `IndexOf` (Array only) and `.Sum` over `Array<decimal>` (the accumulator arm
// at Calls.cpp:3527, distinct from the int arm #6366 fixed).
//
// A 5-terminal matrix sees 4 of the 30 — which is why the family is pinned here
// rather than the two terminals that happened to be sampled.
//
// THE FIX. Every terminal return site in that `isArr` block now emits
// `freeFreshInputTemp()` next to its `freeLazyTemp()`, matching the five that
// already did. The GATE is unchanged
// (`isFreshOwnerlessArrayMaterializerExpr` /
// `isUnboundOwnedBorrowingArrayCallExpr`), so no new population can be
// over-freed: a bound / `using var` / aliased / `move`d / returned receiver is
// not that CallExpr shape, is still freed by its own owner, and is never
// touched here. The CONTROLS below are that direction, and they are the only
// reason an over-free would be loud rather than silent.
//
// NOT PINNED, AND WHY. A row asserting a leak as the EXPECTED outcome is a
// vacuous oracle — it passes while the leak persists and INVERTS the moment the
// leak is fixed, so nothing here asserts one. Two defects in the same
// neighbourhood are NOT this mechanism and are deliberately left to their own
// issues: `LinkedList<T>.Contains` at operator-position 0 (MEASURED 14,056 B on
// a 3-element producer, 8,456 B on a 1-element one — a different arm), and
// `.Length` on a LAZY source (`f.Make().Where(...).Length`, MEASURED 74,336 B —
// the materialized snapshot is never freed on the borrow-position property
// path). `.Length`/`.Count` at operator-position 0 on `Array<T>` is a correct
// sema reject, not a leak.
//
// ORACLE. `runAotChecked` is always ASan and returns nonzero on ANY sanitizer
// finding, so a leak, a use-after-free and a double free each fail the RUN —
// one oracle, both directions, no `leaksAtLeast` (that is a leak oracle, not
// an ASan one). `r.ran` is asserted on every row; there is no skip-absorbing
// guard. Exit codes and `stdout` are both asserted, so a truncated exit code
// cannot hide a wrong value: `acc` counts 101 passes and each row's expected
// `acc` is below 256 except where noted.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Producers over the same element type, so every row is a FAMILY SWAP with
// nothing else changing. `Array<T>` and `Stack<T>` are `bz_array_t`-backed —
// the eager families, the ones that reach a terminal without materializing.
// `LinkedList<T>` is `bz_llist_t`-backed and NEVER array-backed, so every
// terminal on it materializes and #6380 already owns the source: that is the
// control, not a target.
std::string makeArray() {
    return "public class F "
           "  public method Make(): owned Array<int> "
           "    return [1, 2, 3]; "
           "  end Make; "
           "end F; ";
}

std::string makeArray1() {
    return "public class F "
           "  public method Make(): owned Array<int> "
           "    return [1]; "
           "  end Make; "
           "end F; ";
}

std::string makeStack() {
    return "public class F "
           "  public method Make(): owned Stack<int> "
           "    return [1, 2, 3]; "
           "  end Make; "
           "end F; ";
}

std::string makeStack1() {
    return "public class F "
           "  public method Make(): owned Stack<int> "
           "    return [1]; "
           "  end Make; "
           "end F; ";
}

std::string makeLlist() {
    return "public class F "
           "  public method Make(): owned LinkedList<int> "
           "    var l: LinkedList<int> := new LinkedList<int>(); "
           "    l.Add(1); l.Add(2); l.Add(3); "
           "    return l; "
           "  end Make; "
           "end F; ";
}

// The decimal accumulator arm is a SEPARATE site (LlvmBackend_Calls.cpp:3527)
// from the int arm #6366 fixed at :3748, so it needs its own producer and its
// own row: same mechanism, different code path.
std::string makeDecimalArray() {
    return "public class F "
           "  public method Make(): owned Array<decimal> "
           "    return [1.5m, 2.5m]; "
           "  end Make; "
           "end F; ";
}

// 101 passes (`0 .. 100` is inclusive here), so a row whose statement adds `p`
// per pass prints `101 * p` and returns `101 * p mod 100`. `acc` is the printed
// value; the residue is the exit code. Both are asserted, so neither a wrong
// value nor an 8-bit truncation can pass unnoticed.
std::string driver(const std::string& decl, const char* moduleName,
                   const std::string& stmt) {
    return std::string("module ") + moduleName + "; " + decl +
           "public class M "
           "  public static method Main(): int "
           "    var f := new F(); "
           "    var acc := 0; "
           "    for var i in 0 .. 100 do "
           "      " + stmt +
           "      acc := acc + 1; "
           "    end; "
           "    Console.WriteLine($\"{acc}\"); "
           "    return acc mod 100; "
           "  end Main; end M;";
}

// The one assertion every row makes. `expectedAcc` is the MEASURED value of the
// terminal over `[1, 2, 3]` (or `[1]` where the producer says so) — a row that
// merely counted passes would pass even if the terminal returned garbage, which
// is exactly what an over-eager reclaim produces.
void expectReclaimed(const std::string& producer, const char* moduleName,
                     const std::string& stmt, int perPass) {
    JitResult r = runAotChecked(driver(producer, moduleName, stmt));
    const int expectedAcc = 101 * perPass;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expectedAcc % 100);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(std::to_string(expectedAcc)) !=
                      std::string::npos);
}

}  // namespace

// ===========================================================================
// THE FOUR SHAPES — the sample the #6380 review measured: `.Any()` and
// `.First()` with NO lazy operator before them, on the two eager families.
// 12,120 B each on this fix's parent. Each row also pins the terminal's VALUE
// (`Any` true, `First` 1), so an over-eager reclaim that returned garbage from
// a freed backing would fail here too.
// ===========================================================================
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_any_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA1",
                    "if f.Make().Any() then acc := acc + 1; end;", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_first_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA2", "acc := acc + f.Make().First();", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_any_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS1",
                    "if f.Make().Any() then acc := acc + 1; end;", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_first_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS2", "acc := acc + f.Make().First();", 2);
}

// ===========================================================================
// THE REST OF THE MEASURED FAMILY, `Array<T>`. Same shape, same 12,120 B on the
// parent; a 5-terminal matrix would have missed every one of these.
// `Single` needs a ONE-element producer — §5 makes `Single()` throw on a
// 3-element source, and a row that throws on iteration 0 never evaluates the
// terminal again, so it would assert nothing about reclamation.
// ===========================================================================
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_all_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA3",
                    "if f.Make().All(lambda(x) => x > 0) then acc := acc + 1; end;",
                    2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_contains_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA4",
                    "if f.Make().Contains(2) then acc := acc + 1; end;", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_single_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray1(), "MEGA5", "acc := acc + f.Make().Single();", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_last_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA6", "acc := acc + f.Make().Last();", 4);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_min_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA7", "acc := acc + f.Make().Min();", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_max_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA8", "acc := acc + f.Make().Max();", 4);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_average_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    // `Average()` returns a double, so the row asserts it is a real positive
    // value rather than folding it into the int accumulator.
    expectReclaimed(makeArray(), "MEGA9",
                    "var d := f.Make().Average(); if d > 0.0 then acc := acc + 1; end;",
                    2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_aggregate_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA10",
                    "acc := acc + f.Make().Aggregate(0, lambda(a, b) => a + b);",
                    7);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_elementat_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA11",
                    "acc := acc + f.Make().ElementAt(1);", 3);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_firstornull_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA12",
                    "var v := f.Make().FirstOrNull(); if v <> null then acc := acc + v!; end;",
                    2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_lastornull_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA13",
                    "var v := f.Make().LastOrNull(); if v <> null then acc := acc + v!; end;",
                    4);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_singleornull_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray1(), "MEGA14",
                    "var v := f.Make().SingleOrNull(); if v <> null then acc := acc + v!; end;",
                    2);
}

// `IndexOf` is asserted as a SIGN, not an exact index: the row's job is to prove
// the terminal read a LIVE backing, and pinning the index's base would pin a
// separate question about `IndexOf`'s contract that these rows have no opinion
// on. (`IndexOf` is declared only by `Array<T>`, stdlib §4.4 — `Stack<T>` and
// `LinkedList<T>` reject it at sema with a diagnostic pointing at `ToArray()`.)
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_indexof_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGA15",
                    "if f.Make().IndexOf(2) >= 0 then acc := acc + 1; end;", 2);
}

// The decimal accumulator arm — a DIFFERENT site (Calls.cpp:3527) from the int
// arm #6366 fixed (:3748), with the same omission. Pinned separately so the
// decimal path cannot regress behind the int one.
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_decimal_sum_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeDecimalArray(), "MEGA16",
                    "var d := f.Make().Sum(); if d > 0.0m then acc := acc + 1; end;",
                    2);
}

// ===========================================================================
// THE REST OF THE MEASURED FAMILY, `Stack<T>` — the second eager backing. Same
// mechanism, same bytes; `freeUnboundOwnedBorrowingArrayTemp` picks its runtime
// free from the receiver's own backing, so this is the row that proves the
// three-way classification is not array-only.
// ===========================================================================
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_contains_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS3",
                    "if f.Make().Contains(2) then acc := acc + 1; end;", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_single_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack1(), "MEGS4", "acc := acc + f.Make().Single();", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_last_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS5", "acc := acc + f.Make().Last();", 4);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_min_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS6", "acc := acc + f.Make().Min();", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_max_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS7", "acc := acc + f.Make().Max();", 4);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_all_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS8",
                    "if f.Make().All(lambda(x) => x > 0) then acc := acc + 1; end;",
                    2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_firstornull_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGS9",
                    "var v := f.Make().FirstOrNull(); if v <> null then acc := acc + v!; end;",
                    2);
}

// ===========================================================================
// CONTROLS — the shapes that were ALREADY clean and must stay clean. They are
// the only reason an over-eager reclaim is loud rather than silent, and they
// are also the reachability proof in the other direction: `.Sum` and `.Count`
// already called `freeFreshInputTemp` and go through the identical gate, so a
// row that turned red here would mean the gate itself moved.
//
// `Count` and `Sum` at operator-position 0 on an eager family: the two terminals
// #6366 fixed, whose cleanliness is what made the sibling omission visible.
// ===========================================================================
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_count_terminal_of_an_unbound_owned_temp_is_still_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGC1", "acc := acc + f.Make().Count();", 4);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_sum_terminal_of_an_unbound_owned_temp_is_still_reclaimed_spec_28_3) {
    expectReclaimed(makeArray(), "MEGC2", "acc := acc + f.Make().Sum();", 7);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_count_terminal_of_an_unbound_owned_temp_is_still_reclaimed_spec_28_3) {
    expectReclaimed(makeStack(), "MEGC3", "acc := acc + f.Make().Count();", 4);
}

// A LAZY OPERATOR before the terminal: the #6380 path. `lazyTerminal` holds, the
// source is materialized into `lazyTempArr`, and `freeFreshInputTemp` returns
// early on exactly that shape — so the two reclaims can never both fire. These
// rows are the mechanical proof of that mutual exclusion.
// ===========================================================================
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_lazy_operator_then_any_is_reclaimed_by_the_lazy_source_spec_28_3) {
    expectReclaimed(makeArray(), "MEGC4",
                    "if f.Make().Where(lambda(x) => x > 0).Any() then acc := acc + 1; end;",
                    2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           array_lazy_operator_then_first_is_reclaimed_by_the_lazy_source_spec_28_3) {
    expectReclaimed(makeArray(), "MEGC5",
                    "acc := acc + f.Make().Select(lambda(x) => x).First();", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           stack_lazy_operator_then_any_is_reclaimed_by_the_lazy_source_spec_28_3) {
    expectReclaimed(makeStack(), "MEGC6",
                    "if f.Make().Where(lambda(x) => x > 0).Any() then acc := acc + 1; end;",
                    2);
}

// `LinkedList<T>` at operator-position 0 is never array-backed, so EVERY terminal
// on it materializes and #6380's ownership transfer is the whole reclaim — this
// fix adds nothing to it. Pinned so a regression there is attributed correctly
// rather than blamed on this change. (`Contains` is deliberately absent: it
// leaks 14,056 B by a different mechanism, and a row asserting that leak would
// be the vacuous oracle this file exists to avoid.)
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           linkedlist_any_terminal_of_an_unbound_owned_temp_stays_reclaimed_spec_28_3) {
    expectReclaimed(makeLlist(), "MEGC7",
                    "if f.Make().Any() then acc := acc + 1; end;", 2);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           linkedlist_firstornull_terminal_of_an_unbound_owned_temp_stays_reclaimed_spec_28_3) {
    // `LinkedList<T>.First()` is declared `: T?` (stdlib §5 — an unordered
    // source cannot promise a first element), so the row uses the `*OrNull`
    // spelling. Using `.First()` here would be a COMPILE failure, not a leak,
    // and `r.ran` is what would catch that — which is why it is asserted.
    expectReclaimed(makeLlist(), "MEGC8",
                    "var v := f.Make().FirstOrNull(); if v <> null then acc := acc + v!; end;",
                    2);
}

// ===========================================================================
// CONTROLS — the OVER-FREE direction, and the only reason that direction is
// loud. `isUnboundOwnedBorrowingArrayCallExpr` matches a CALL used DIRECTLY as
// the receiver, so a receiver bound to a local is never matched, is never
// reclaimed by the terminal, and is discharged by its own owner at scope end.
//
// These rows READ THE RECEIVER BACK AFTER the terminal has run. If the reclaim
// ever reached a bound receiver, that read is a use-after-free of a freed
// backing — ASan reports it and `runAotChecked` returns nonzero for it exactly
// as it does for a leak, so one oracle covers both. Without these rows an
// over-free would be a silent-looking green.
// ===========================================================================
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           bound_array_receiver_is_never_reclaimed_by_an_eager_terminal_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module MEGO1; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var a := f.Make(); "
        "      acc := acc + a.First(); "
        "      acc := acc + a.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // 101 x (1 + 3) = 404
    BEZEL_EXPECT_TRUE(r.stdoutText.find("404") != std::string::npos);
}

BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           bound_stack_receiver_is_never_reclaimed_by_an_eager_terminal_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module MEGO2; ") + makeStack() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var s := f.Make(); "
        "      if s.Any() then acc := acc + 1; end; "
        "      acc := acc + s.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // 101 x (1 + 3) = 404
    BEZEL_EXPECT_TRUE(r.stdoutText.find("404") != std::string::npos);
}

// The same over-free direction when the bound receiver is consumed by a LAZY
// operator, so `freeLazyTemp` fires and `freeFreshInputTemp` must stay inert:
// the two are complementary on `lazyTempArr`, and this row fails if they ever
// both fire on one receiver.
BEZEL_TEST(eager_terminal_unbound_owned_reclaim,
           bound_array_receiver_survives_a_lazy_operator_chain_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module MEGO3; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var a := f.Make(); "
        "      acc := acc + a.Where(lambda(x) => x > 0).First(); "
        "      acc := acc + a.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // 101 x (1 + 3) = 404
    BEZEL_EXPECT_TRUE(r.stdoutText.find("404") != std::string::npos);
}
