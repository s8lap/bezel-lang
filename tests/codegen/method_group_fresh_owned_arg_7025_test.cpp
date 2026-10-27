// fix #7025 (follow-up, §28.3 / §27.6, v0.343): a FRESH OWNED argument
// passed to a FUNCTION VALUE is §28.3's unbound owned temporary, and is
// discharged at the end of the full expression — the function-value invoke
// path must do it.
//
// THE RULE. §28.3 "Unbound owned temporaries": a fresh owned value "not
// bound to any name, not `move`d, and not returned, but used only by
// *borrowing* operations within the expression — as the receiver of a further
// call, as a non-`owned` argument, …" is "disposed at the **end of the full
// expression** in which it is produced, after every use of it within that
// expression has completed".
//
// THE DEFECT. The FUNCTION-VALUE invoke path (`Emitter::emitClosureCall`)
// builds its OWN argument list — it does not route through `buildOrderedArgs`
// — so it never discharged the temporary. The value was owned by nobody and
// leaked.
//
// MEASURED (AddressSanitizer/LeakSanitizer) on the reviewer's probe
//
//   var f: (Item) -> () := il.Add;
//   f(new Item());
//
// ⇒ "Direct leak of 16 byte(s) in 1 object(s)", while the byte-identical
// DIRECT call `il.Add(new Item());` is clean (the borrowing-collection
// `Add` arm discharges it). At the stack base the same program SEGV'd, so
// the `IList<T>` arm had traded a CRASH for a LEAK — a fix that makes MORE
// programs compile is a fix that owns the new program's defects.
//
// THE ROWS BELOW:
//
//   * the function-VALUE invoke with a fresh owned class argument — the
//     shape that leaked; it must run, print the collection's count, and
//     leak NOTHING;
//   * the CONTROL: the DIRECT call of the same member with the same fresh
//     argument — it already disposed the temporary and must keep doing so;
//   * the CONTROL: a plain LAMBDA function value with the same fresh owned
//     class argument — the leak was GENERAL to the function-value invoke
//     path, not specific to a bound-method thunk.
//
// Each row is pinned with `runAotChecked`, so the AOT leg runs the program
// under AddressSanitizer/LeakSanitizer: the pre-fix leak fails the row.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The reviewer's probe, minus the `Console.WriteLine` of the count (the
// count is asserted below).
constexpr char kItemClass[] =
    "module M; "
    "import System; "
    "public class Item "
    "  public method Get(): int return 4; end Get; "
    "end Item; "
    "class C "
    "  public static method Main(): int "
    "    var a: Array<Item> := []; "
    "    var il: IList<Item> := a; ";

}  // namespace

// The FUNCTION-VALUE invoke: the member is formed into a function value and
// the fresh owned element is passed to that value.
//
// PRE-FIX: exit 1, stdout "1", "LeakSanitizer: detected memory leaks /
// Direct leak of 16 byte(s) in 1 object(s)".
BEZEL_TEST(method_group_fresh_owned_arg_7025,
           spec_28_3_fresh_owned_argument_to_a_function_value_is_disposed) {
    auto r = runAotChecked(
        std::string(kItemClass) +
        "    var f: (Item) -> () := il.Add; "
        "    f(new Item()); "
        "    Console.WriteLine(il.Count); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// The CONTROL: the DIRECT call of the same member with the same fresh owned
// argument. It already disposed the temporary (§28.11's "a retained fresh
// `Add` argument" arm discharges it) — the fix must not change it.
BEZEL_TEST(method_group_fresh_owned_arg_7025,
           spec_28_3_fresh_owned_argument_to_a_direct_call_is_unchanged) {
    auto r = runAotChecked(
        std::string(kItemClass) +
        "    il.Add(new Item()); "
        "    Console.WriteLine(il.Count); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// The CONTROL: a plain LAMBDA function value with the same fresh owned class
// argument. The leak was GENERAL to the function-value invoke path — a
// lambda literal is not a bound-method thunk — so the fix covers it too.
//
// PRE-FIX: exit 1, "Direct leak of 16 byte(s) in 1 object(s)".
BEZEL_TEST(method_group_fresh_owned_arg_7025,
           spec_28_3_fresh_owned_argument_to_a_lambda_is_disposed) {
    auto r = runAotChecked(
        std::string(kItemClass) +
        "    var f: (Item) -> () := lambda(x: Item) => do return; end; "
        "    f(new Item()); "
        "    Console.WriteLine(il.Count); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}
