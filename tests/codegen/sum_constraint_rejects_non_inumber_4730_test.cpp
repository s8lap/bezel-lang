// Regression tests for #4730 — `Sum()` over-accepted `bool`, `char`, and
// nullable `T?` element types despite the stdlib §5
//   extension IEnumerable<T> where T: INumber<T>
// constraint and §2.19's enumeration of the INumber conformers.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §5 (v0.286) declares
//     public method Sum(): owned T;
// under the `extension IEnumerable<T> where T: INumber<T>` block, and spec
// §2.19 names the INumber<T> conformers exactly — byte, sbyte, short, ushort,
// int, uint, long, ulong, float, double, decimal. `bool` conforms to no
// interface at all; `char` conforms to `ISteppable<T>` / `IParsable<T>` only
// (NOT `INumber<T>` — `+` is not defined on char); and a nullable `T?`
// does not satisfy `T: INumber<T>` (the constraint demands a non-null T).
//
// THE DEFECT WAS A SILENT WRONG-CODE ACCEPT.  The no-selector Sum() arm in
// sema (src/sema/ExprTypecheck_Calls.cpp) fell through to a legacy `int`
// default for any element type that didn't already satisfy
// `isNoSelectorAggregationElem`, so `Array<bool>.Sum()` was typed `int`
// and codegen built `trunc i1 to i32` (LLVM IR verifier: "DestTy too big
// for Trunc"); `Array<char>.Sum()` was typed `int` and ran, silently
// returning 294 for the three letters; and `Array<int?>.Sum()` was typed
// `int` against a `{i8, i32}` nullable layout and codegen built
// `add i32, { i8, i32 }` (LLVM IR verifier: "Both operands to a binary
// operator are not of the same type!"). None of those failures are the
// "Sum of int 6" the user could ever have wanted — each is an
// IR-verifier fault or a meaningless garbage value.
//
// SPEC-CITED TEST CASES.  The three element types named in the issue
// each round-trip a distinct codegen failure mode:
//
//   * `Array<bool>` — IR-verifier fault (trunc i1 to i32).
//   * `Array<char>` — silent wrong code (sum of three letters prints
//     97+98+99 = 294).
//   * `Array<int?>` — IR-verifier fault (binary-operator type mismatch).
//
// All three must be REJECTED at sema with a diagnostic that names the
// constraint (`T: INumber<T>`) and the offending element type. A fix that
// compiles them silently (e.g. with an int default) was the original
// defect and is not acceptable.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Wrap a body in the standard module + class harness that runAot expects.
std::string main_(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + "  return 0; end Main; end C;";
}

// Diagnostic TEXT: these shapes previously COMPILED, so "did not run" alone
// would also be satisfied by an unrelated IR-verifier crash. The needle pins
// the constraint name (`INumber<T>`), the stdlib §5 citation, and the
// offending element type so a fix that catches the call for the wrong
// reason — e.g. "no member `Sum` on `Array<bool>`" — still fails.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// #4730 / stdlib §5 + spec §2.19 (v0.307): Sum() over Array<bool> must be
// REJECTED at sema — bool conforms to no INumber<T> interface, and the
// + operator is not defined on bool. The pre-fix compiler accepted and
// emitted `trunc i1 to i32`, which the IR verifier rejected with
// "DestTy too big for Trunc".
BEZEL_TEST(sum_constraint_rejects_non_inumber_4730,
           sum_on_array_bool_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(
        "  using var arr := new Array<bool>(); "
        "  arr.Add(true); arr.Add(false); arr.Add(true); "
        "  Console.WriteLine(arr.Sum()); "),
        "INumber<bool>"));
}

// #4730 / stdlib §5 + spec §2.19: Sum() over Array<char> must be REJECTED.
// char conforms to ISteppable/IParsable only, NOT to INumber — the + operator
// is not defined on char. The pre-fix compiler accepted and silently printed
// 294 (= 'a' + 'b' + 'c' as int code points), which is not what the user
// meant by "sum of these characters".
BEZEL_TEST(sum_constraint_rejects_non_inumber_4730,
           sum_on_array_char_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(
        "  using var arr := new Array<char>(); "
        "  arr.Add('a'); arr.Add('b'); arr.Add('c'); "
        "  Console.WriteLine(arr.Sum()); "),
        "INumber<char>"));
}

// #4730 / stdlib §5 + spec §2.19: Sum() over Array<int?> must be REJECTED.
// A nullable element does not satisfy `T: INumber<T>` — the constraint
// demands a non-null T. The pre-fix compiler accepted and emitted
// `add i32, { i8, i32 }`, which the IR verifier rejected with "Both operands
// to a binary operator are not of the same type!".
BEZEL_TEST(sum_constraint_rejects_non_inumber_4730,
           sum_on_array_nullable_int_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(
        "  using var arr := new Array<int?>(); "
        "  arr.Add(1); arr.Add(null); arr.Add(2); arr.Add(3); "
        "  Console.WriteLine(arr.Sum()); "),
        "INumber<int?>"));
}

// #4730 / stdlib §5 + spec §2.19: an int element is INumber<int> and must
// STILL be accepted — the fix must not over-reject. Control: Sum() over
// Array<int> still works and prints the correct sum.
BEZEL_TEST(sum_constraint_rejects_non_inumber_4730,
           sum_on_array_int_still_accepted) {
    auto r = runAot(main_(
        "  using var arr := new Array<int>(); "
        "  arr.Add(1); arr.Add(2); arr.Add(3); "
        "  Console.WriteLine(arr.Sum()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    // The post-fix Sum() result is the element's own type — int in this case,
    // so Console.WriteLine(int) prints "6".
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}
