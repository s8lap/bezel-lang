// Codegen fix #626 / §25.1 v0.135 / §4.4 / §4.8 — compound assignment
// (`+=`, `-=`) via a BUILT-IN-collection indexer where the element type has
// a user-defined binary operator overload:
//
//   arr[0] += new Money(50)   // Array<Money>, Money has operator +
//   d["k"] += new Money(50)   // Dictionary<string, Money>, ditto
//
// Before the fix, both shapes routed through lowerCollectionIndexerCompound's
// emitCompoundOp path.  emitCompoundOp uses llvm.sadd.with.overflow on the
// struct's LLVM struct type, which LLVM rejects at IR verification (struct
// type cannot be an operand of sadd.with.overflow) → bezelc error "IR
// verification failed".
//
// Fix (LlvmBackend_Assign.cpp, lowerCollectionIndexerCompound): before calling
// emitCompoundOp, check the element type's declaration for a `static operator
// <op>` with two parameters (mirrors the #610 check in lowerStmt).  If found:
//   1. Read the element via lowerCollectionIndexerGet.
//   2. Evaluate the RHS.
//   3. Call the operator overload with (cur, rhs).
//   4. Write the result back via lowerCollectionIndexerSet.
// The existing emitCompoundOp path is reached only when no overload is found
// (numeric elements such as int and float).
//
// Spec authority: §25.1 (compound-assignment desugar rule, v0.135),
// §4.4 (Array<T> indexer get/set), §4.8 (Dictionary<K,V> indexer get/set).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Common Money record preamble used by most tests.
const char kMoneyRecord[] =
    "record Money(Amount: int) "
    "  public static method operator +(a: Money, b: Money): Money => "
    "    new Money(a.Amount + b.Amount); "
    "  public static method operator -(a: Money, b: Money): Money => "
    "    new Money(a.Amount - b.Amount); "
    "end Money; ";
} // namespace

// ---------------------------------------------------------------------------
// Array<Money> += — §4.4 / §25.1
// ---------------------------------------------------------------------------

// Primary repro for #626 (array shape): arr[0] += new Money(50) where Money
// has operator +.  Before the fix this emitted malformed IR and crashed bezelc.
BEZEL_TEST(builtin_collection_compound_overload_626, array_money_plus_equals_4_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(100)); "
        "    arr[0] += new Money(50); "
        "    return arr[0].Amount; "  // expected 150
        "  end Main; "
        "end Program;"),
        150);
}

// §25.1 / §4.4: `-=` desugars to operator - overload via indexer.
BEZEL_TEST(builtin_collection_compound_overload_626, array_money_minus_equals_4_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(100)); "
        "    arr[0] -= new Money(42); "
        "    return arr[0].Amount; "  // expected 58
        "  end Main; "
        "end Program;"),
        58);
}

// §25.1 / §4.4: compound += inside a loop accumulates via operator overload.
BEZEL_TEST(builtin_collection_compound_overload_626, array_money_plus_loop_4_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(0)); "
        "    var i := 0; "
        "    while i < 5 do "
        "      arr[0] += new Money(10); "
        "      i++; "
        "    end; "
        "    return arr[0].Amount; "  // expected 50
        "  end Main; "
        "end Program;"),
        50);
}

// §25.1 / §4.4: compound += at a non-zero index updates only that slot.
BEZEL_TEST(builtin_collection_compound_overload_626, array_money_nonzero_index_4_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(100)); "  // slot 0
        "    arr.Add(new Money(200)); "  // slot 1
        "    arr[1] += new Money(50); "
        "    Console.WriteLine(arr[0].Amount + arr[1].Amount); return 0; "  // 100 + 250 = 350
        "  end Main; "
        "end Program;"),
        "350\n");
}

// ---------------------------------------------------------------------------
// Dictionary<string, Money> += — §4.8 / §25.1
// ---------------------------------------------------------------------------

// Primary repro for #626 (dict shape): d["k"] += new Money(50) where Money
// has operator +.  Before the fix this also emitted malformed IR.
BEZEL_TEST(builtin_collection_compound_overload_626, dict_str_money_plus_equals_4_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, Money>(); "
        "    d.Add(\"k\", new Money(100)); "
        "    d[\"k\"] += new Money(50); "
        "    return d[\"k\"].Amount; "  // expected 150
        "  end Main; "
        "end Program;"),
        150);
}

// §25.1 / §4.8: `-=` on a Dictionary<string, Money> desugars to operator -.
BEZEL_TEST(builtin_collection_compound_overload_626, dict_str_money_minus_equals_4_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, Money>(); "
        "    d.Add(\"k\", new Money(100)); "
        "    d[\"k\"] -= new Money(42); "
        "    return d[\"k\"].Amount; "  // expected 58
        "  end Main; "
        "end Program;"),
        58);
}

// ---------------------------------------------------------------------------
// Regression: int elements keep the numeric intrinsic path — §4.4 / §4.8
// ---------------------------------------------------------------------------

// §4.4 / §25.1: Array<int> += must still use the overflow-checked intrinsic.
BEZEL_TEST(builtin_collection_compound_overload_626, array_int_plus_regression_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr[0] += 5; "
        "    return arr[0]; "  // 15
        "  end Main; "
        "end C;"),
        15);
}

// §4.8 / §25.1: Dictionary<string,int> += must still work numerically.
BEZEL_TEST(builtin_collection_compound_overload_626, dict_str_int_plus_regression_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"k\", 10); "
        "    d[\"k\"] += 5; "
        "    return d[\"k\"]; "  // 15
        "  end Main; "
        "end C;"),
        15);
}

// ---------------------------------------------------------------------------
// §17.6 (#1299): the index subexpression is evaluated EXACTLY ONCE
// ---------------------------------------------------------------------------
//
// When the element type has a user-defined `operator +`, the array OP= path
// desugared to get → overload → set, but the write-back went through
// lowerCollectionIndexerSet(*ix, …), which RE-lowers ix->target AND
// ix->indices[0].value — so a side-effecting index (`arr[Idx()] += …`) was
// evaluated twice: the getter read one slot and the setter wrote a different
// one, silently corrupting data. §17.6 requires each LHS subexpression to be
// "evaluated exactly once". The fix reuses the already-evaluated recv/idx for
// the write-back (matching the non-overload path). Idx() counts its calls via
// the static N.

// The side-effecting index must fire exactly once → getter and setter target
// the SAME slot. Idx() returns 0 on its (single) call, so arr[0] becomes
// Money(10+100)=110 and arr[1] stays 20.  110*1000 + 20 = 110020.
BEZEL_TEST(builtin_collection_compound_overload_626, array_overload_index_evaluated_once_17_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; ") + kMoneyRecord +
        "class C "
        "  public static N: int; "
        "  public static method Idx(): int "
        "    var r := N; N := N + 1; return r; "
        "  end Idx; "
        "  public static method Main(): int "
        "    N := 0; "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(10)); arr.Add(new Money(20)); "
        "    arr[Idx()] += new Money(100); "
        "    Console.WriteLine(arr[0].Amount * 1000 + arr[1].Amount); return 0; "  // 110020
        "  end Main; "
        "end C;"),
        "110020\n");
}

// Direct assertion that Idx() is called exactly once (§17.6): Main returns N,
// the call count, which must be 1 (was 2 before the fix).
BEZEL_TEST(builtin_collection_compound_overload_626, array_overload_index_call_count_is_one_17_6) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class C "
        "  public static N: int; "
        "  public static method Idx(): int "
        "    var r := N; N := N + 1; return r; "
        "  end Idx; "
        "  public static method Main(): int "
        "    N := 0; "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(10)); arr.Add(new Money(20)); "
        "    arr[Idx()] += new Money(100); "
        "    return N; "  // exactly one call
        "  end Main; "
        "end C;"),
        1);
}

// The non-overload (intrinsic int) path was already single-evaluation; guard it
// stays so — Idx() fires once, arr[0] becomes 110, arr[1] stays 20.
BEZEL_TEST(builtin_collection_compound_overload_626, array_int_index_evaluated_once_17_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static N: int; "
        "  public static method Idx(): int "
        "    var r := N; N := N + 1; return r; "
        "  end Idx; "
        "  public static method Main(): int "
        "    N := 0; "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); "
        "    arr[Idx()] += 100; "
        "    Console.WriteLine(arr[0] * 1000 + arr[1] + N); return 0; "  // 110020 + 1 = 110021
        "  end Main; "
        "end C;"),
        "110021\n");
}
