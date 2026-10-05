// Regression test for #5359 — `ChannelSender<T>.Send` fails to lower when its
// argument is an inline array literal written at the call site.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   Stdlib §6.9: `ChannelSender<T>.Send(owned item: T)` — stdlib §15.4
//   (collection literals) admits a collection literal "via the parameter type of
//   a method call", so `tr.Send([1, 2, 3])` on a `ChannelSender<[3]int>` is
//   normative syntax.
//
// THE DEFECT.
//
//   `lowerExpr` returns null for a `CollectionLiteralExpr` because it is not a
//   general expression value (§15.4): the literal must be materialized against
//   a target type. `buildOrderedArgs` handles this for ordinary call arguments
//   by materializing an inline-array literal element-by-element into a stack
//   alloca. The `ChannelSender.Send` and `SendAsync` dispatch arms in
//   `tryLowerChannelAndDomainCall` called `lowerExpr` directly on the argument
//   and returned nullptr when it encountered a literal — which reached the
//   "no dispatch arm" backstop in `lowerCallDispatch` and produced the
//   misleading error.
//
// THE FIX.
//
//   Handle `CollectionLiteralExpr` in the `Send` and `SendAsync` arms exactly
//   as `buildOrderedArgs` does: detect an inline-array target type, allocate
//   a stack slot, materialize each element into it, and load the aggregate.
//
// ROWS.
//
//   USER ROW 1: `tr.Send([1, 2, 3])` — the exact repro from #5359. Fails at
//   r23-c28 with the "no dispatch arm" diagnostic; passes with the fix.
//   USER ROW 2: `await tr.SendAsync([4, 5, 6])` — the async twin; same fix.
//   CONTROL ROW 1: `var a: [3]int := [1, 2, 3]; tr.Send(a);` — must keep
//   working; proves the fix did not break the named-variable path.
//   CONTROL ROW 2: `var b: [3]int := [4, 5, 6]; await tr.SendAsync(b);` —
//   the async control.
//
// Naming: `spec_<section>` names the section the row is derived from.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Oracle: plain runAot (compile + native run + exit code + exact stdout).
// runAotChecked is not used because these rows verify LOWERING correctness,
// not memory-safety (no leaks to detect — the channel transfers ownership and
// Receive disposes the item). runAot is the strict oracle for lowering.
void expectClean(const std::string& src, const std::string& want) {
    bezel::test::JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// §15.4 / §6.9 / §30.15: the exact repro from #5359. An inline array literal
// as the argument to `ChannelSender<[3]int>.Send`. Pre-fix: "no dispatch arm
// emitted any code for this call". Post-fix: the literal is materialized into a
// stack alloca and passed to `encodeChannelItemWord` correctly.
BEZEL_TEST(channel_send_inline_array_literal, user_send_inline_literal_spec_15_4) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  tr.Send([1, 2, 3]); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  return 0; end Main; end C;",
                "1,2,3\n");
}

// §15.4 / §6.9 / §30.15: the async twin. `SendAsync` uses the identical
// item-word encoding; the same fix applies.
BEZEL_TEST(channel_send_inline_array_literal,
           user_send_async_inline_literal_spec_15_4) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  await tr.SendAsync([4, 5, 6]); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  return 0; end Main; end C;",
                "4,5,6\n");
}

// CONTROL: the named-variable path must remain functional.
BEZEL_TEST(channel_send_inline_array_literal,
           control_send_named_variable_spec_6_9) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var a: [3]int := [1, 2, 3]; "
                "  tr.Send(a); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  return 0; end Main; end C;",
                "1,2,3\n");
}

// CONTROL: the async named-variable path.
BEZEL_TEST(channel_send_inline_array_literal,
           control_send_async_named_variable_spec_6_9) {
    expectClean("module M; import System.Threading; "
                "class C public static async method Main(): Task<int> "
                "  using var cr := new Channel<[3]int>(); "
                "  using var tr := cr.NewSender(); "
                "  var b: [3]int := [4, 5, 6]; "
                "  await tr.SendAsync(b); "
                "  var o := await cr.Receive(); "
                "  Console.WriteLine($\"{o[0]},{o[1]},{o[2]}\"); "
                "  return 0; end Main; end C;",
                "4,5,6\n");
}
