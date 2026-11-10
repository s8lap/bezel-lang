// Regression tests for stdlib §5 `Sum(selector)` over a NON-decimal element
// whose selector returns `decimal` (fix #8388; stdlib §5 / §9.3, v0.355).
//
// stdlib §5 (v0.355) declares
//     public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
// so the selector's RESULT type U is the accumulator's type: for U = decimal
// the fold is a decimal addition, and the sum of 3m and 4m is 7m.
//
// The `Sum` arm's DECIMAL path loaded the fold's element as the DECIMAL struct
// — the NO-SELECTOR form's shape (`Array<decimal>.Sum()`, where the element IS
// the decimal) — and called the selector closure with a decimal-shaped
// argument. For a receiver whose element is NOT a decimal (`Array<Item>`) the
// closure's real parameter is the ELEMENT (T), so the callee received a
// TRUNCATED element (a 20-byte record read as a 16-byte decimal, the rest of
// the record read off the stack) and the fold added whatever garbage came
// back:
//
//   items.Sum(lambda(i) => i.Qty * i.Price) over [Item(2, 1.50m), Item(1, 4.00m)]
//       -> 34 on the base compiler; 7 is the correct sum (2*1.50 + 1*4.00).
//   items.Sum(lambda(i) => i.Price)          over the same array
//       -> 0 on the base compiler; 5.5 is the correct sum (1.50 + 4.00).
//
// The fix loads the element at the ELEMENT's own LLVM type and calls the
// closure with that type — the discipline the `Min`/`Max` decimal-selector arm
// already uses (fix #8108) and the discipline `callClosure` uses.
//
// stdlib §9.3 (v0.355) fixes the decimal's rendering: "scale is not part of a
// decimal's value" — `1.50m` and `1.5m` are one value and both render `1.5`,
// `7.00m` renders `7`. Every expectation below is therefore the value's
// shortest exact form, NOT the operand's scale.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// M1 — THE DEFECT. The selector's result type is `decimal` (int * decimal), so
// the fold is a decimal addition: 2*1.50m = 3m and 1*4.00m = 4m, and the sum
// is 7m. The base compiler answered 34 (the garbage the truncated element
// produced); 7 is the value stdlib §5 requires.
BEZEL_TEST(sum_decimal_selector_8388, decimal_selector_product_folds_the_products) {
    // stdlib §5, v0.355 / spec §11.8, §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)]; "
        "    Console.WriteLine(items.Sum(lambda(i) => i.Qty * i.Price)); return 0; "
        "  end Main; "
        "end C;"), "7");
}

// M5 — THE ISOLATING ROW: a decimal selector that is a plain field read, with
// NO multiplication in it. The base compiler answered 0 here too, which says
// the defect is NOT in the `int * decimal` multiply (that multiply alone is
// right — see the multiply-alone row below) but in how the fold hands the
// ELEMENT to the selector. 1.50m + 4.00m = 5.5m.
BEZEL_TEST(sum_decimal_selector_8388, decimal_selector_that_is_a_plain_field) {
    // stdlib §5, v0.355 / spec §11.8, §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)]; "
        "    Console.WriteLine(items.Sum(lambda(i) => i.Price)); return 0; "
        "  end Main; "
        "end C;"), "5.5");
}

// M6 — the ONE-element fold: one product (2 * 1.50m = 3m) and nothing else.
// The base compiler answered 0.03 here (the truncated element read as a
// decimal with the record's scale). A one-element fold must answer 3.
BEZEL_TEST(sum_decimal_selector_8388, one_element_fold_answers_the_one_product) {
    // stdlib §5, v0.355 / spec §11.8, §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.50m)]; "
        "    Console.WriteLine(items.Sum(lambda(i) => i.Qty * i.Price)); return 0; "
        "  end Main; "
        "end C;"), "3");
}

// M8 — the shape the issue reports ("the study saw 0 when the call is inside a
// group"): the fold inside an `if` group. The base compiler answered 0 here;
// the value is the same 7 as the unnested call — a group must not change it.
BEZEL_TEST(sum_decimal_selector_8388, decimal_selector_inside_a_group) {
    // stdlib §5, v0.355 / spec §11.8, §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)]; "
        "    var flag := true; "
        "    if flag then "
        "      Console.WriteLine(items.Sum(lambda(i) => i.Qty * i.Price)); "
        "    end if; "
        "    return 0; "
        "  end Main; "
        "end C;"), "7");
}

// M10 — the same fold over THREE-decimal prices (1.500m, 4.000m): 3m + 4m = 7m
// again. The base compiler answered 0. §9.3 makes scale not part of a
// decimal's value, so the answer is `7` — the same value the two-decimal row
// gives, and the row that pins the fold as scale-INDEPENDENT (a fold that
// carried the operands' scale would answer 7.000, which §9.3's rendering
// forbids: `7.000m` renders `7`).
BEZEL_TEST(sum_decimal_selector_8388, three_decimal_places_fold_to_the_same_value) {
    // stdlib §5 / §9.3, v0.355
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.500m), new Item(1, 4.000m)]; "
        "    Console.WriteLine(items.Sum(lambda(i) => i.Qty * i.Price)); return 0; "
        "  end Main; "
        "end C;"), "7");
}

// M9 — THE SIBLING FOLD, and the row that says WHERE the defect is: the same
// fold written as `Aggregate(0m, lambda(acc, i) => acc + i.Qty * i.Price)`
// answers 7 on the BASE compiler too, so the shared decimal addition (and the
// `int * decimal` multiply it folds) are right — the defect is in the `Sum`
// arm's own accumulation. A control, NOT a failing row.
BEZEL_TEST(sum_decimal_selector_8388, aggregate_sibling_fold_is_unchanged) {
    // stdlib §5, v0.355
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)]; "
        "    Console.WriteLine(items.Aggregate(0m, lambda(acc, i) => acc + i.Qty * i.Price)); "
        "    return 0; "
        "  end Main; "
        "end C;"), "7");
}

// M2 — the `int` selector is untouched: 2 + 1 = 3.
BEZEL_TEST(sum_decimal_selector_8388, int_selector_is_unchanged) {
    // stdlib §5, v0.355 / spec §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Item(Qty: int, Price: decimal); class C "
        "  public static method Main(): int "
        "    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)]; "
        "    Console.WriteLine(items.Sum(lambda(i) => i.Qty)); return 0; "
        "  end Main; "
        "end C;"), "3");
}

// M3 — the NO-SELECTOR decimal path is untouched: `Array<decimal>.Sum()` over
// [1.5m, 2.5m] is 4m (the element IS the decimal, the shape the pre-fix load
// type was written for).
BEZEL_TEST(sum_decimal_selector_8388, no_selector_decimal_array_is_unchanged) {
    // stdlib §5 / §5.1, v0.355
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var ds: Array<decimal> := [1.5m, 2.5m]; "
        "    Console.WriteLine(ds.Sum()); return 0; "
        "  end Main; "
        "end C;"), "4");
}

// M4 — the `int * decimal` multiply ALONE is untouched: 2 * 1.50m = 3m. This
// is the row that says the defect is in the fold's accumulator path and not
// in the multiplication.
BEZEL_TEST(sum_decimal_selector_8388, int_times_decimal_multiply_alone_is_unchanged) {
    // stdlib §9.3 / spec §22.6, v0.355
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var q: decimal := 2 * 1.50m; "
        "    Console.WriteLine(q); return 0; "
        "  end Main; "
        "end C;"), "3");
}

// M7 — the `float` selector path is untouched: 1.5f * 1.0f + 2.5f * 1.0f = 4.
BEZEL_TEST(sum_decimal_selector_8388, float_selector_is_unchanged) {
    // stdlib §5, v0.355 / spec §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var fs: Array<float> := [1.5f, 2.5f]; "
        "    Console.WriteLine(fs.Sum(lambda(x) => x * 1.0f)); return 0; "
        "  end Main; "
        "end C;"), "4");
}

// M7 — the `double` selector path is untouched: 1.5 * 1.0 + 2.5 * 1.0 = 4.
BEZEL_TEST(sum_decimal_selector_8388, double_selector_is_unchanged) {
    // stdlib §5, v0.355 / spec §22.6
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var ds: Array<double> := [1.5, 2.5]; "
        "    Console.WriteLine(ds.Sum(lambda(x) => x * 1.0)); return 0; "
        "  end Main; "
        "end C;"), "4");
}
