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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
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
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kMoneyRecord +
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new Array<Money>(); "
        "    arr.Add(new Money(100)); "  // slot 0
        "    arr.Add(new Money(200)); "  // slot 1
        "    arr[1] += new Money(50); "
        "    return arr[0].Amount + arr[1].Amount; "  // 100 + 250 = 350
        "  end Main; "
        "end Program;"),
        350);
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
