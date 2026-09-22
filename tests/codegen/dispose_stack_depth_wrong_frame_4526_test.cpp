// Codegen — a recorded declaration depth may only index the `disposeStack_`
// of the body that recorded it (§27.5 / §28.3 / §28.9, #4526).
//
// Spec §28.3: an owned local's disposal fires "at the end of the enclosing
// block" — the block of the frame that DECLARES it. Spec §27.5: "A lambda may
// reference variables from its enclosing scope. Captured variables are
// heap-allocated in a closure object" — so a captured local is visible inside
// the lambda's body while remaining the DECLARING frame's to dispose.
//
// THE DEFECT. `declScopeDepth_` is module-wide and never cleared, while
// `disposeStack_` is cleared per body (`lowerLambda`, every async resume /
// iterator moveNext / thunk prolog). #4523 proved that indexing the second
// with a depth recorded into the first by a DIFFERENT body is a reachable
// use-after-free, and introduced `declScopeDepthFn_` — the owning function
// recorded beside every depth. It gated the one site it was filed for.
//
// `lowerByteArrayFactoryArg` (#998 / §28.3) was not gated. It registers the
// caller's `Array<byte>` for scope-end disposal so an FFI factory may merely
// borrow the bytes, and it picks the scope by `declScopeDepth_[loc->decl]`.
// Its only frame check is `locals_.find(loc->decl)` — which is NOT a frame
// check: `locals_` in a lambda body holds the CAPTURED decls too. So for
//
//     var b := new Array<byte>(4, byte(7));
//     var f := lambda(): int => do
//       using var rb := RawBytes.From(b);   (* b is CAPTURED here *)
//       return rb.Length;
//     end;
//
// the lambda's body indexed ITS OWN freshly-cleared `disposeStack_` with
// `Main`'s depth for `b` and registered `Main`'s array for disposal at the
// LAMBDA's scope end. Measured on origin/main (6c05b4cf), `bezelc`:
// `b.Count` reads 0 after the first call to `f`, a second `RawBytes.From(b)`
// copies nothing, and `b[0]` raises `System.IndexOutOfRangeException` — the
// caller's collection is torn down while the caller still owns it.
//
// THE FIX is structural rather than another inline conjunct: one accessor
// (`declScopeDisposeIndex`) that returns an index ONLY when the depth was
// recorded by the body now holding the stack, so a future consumer cannot
// take the index without the check; a companion predicate
// (`declScopeDepthFromOtherFn`) for the consumer whose right answer on a
// foreign local is to register nothing at all rather than to fall back to
// its own innermost scope — falling back would dispose the caller's array
// one scope later, not stop disposing it.
//
// `LlvmBackend_Exprs2.cpp`'s #969 `declaredShallower` test is deliberately
// NOT routed through the accessor: it only COMPARES two depths, which
// `declScopeDepthFn_`'s own declaration documents as safe across bodies.
//
// ORACLE. `runAotChecked` (strict, LSan on): post-fix `b` is disposed exactly
// once, by `Main`, so the strict oracle is the right one and leak-freedom is
// part of what is pinned — the fix must not trade a premature disposal for a
// leak.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ============================================================================
// Row 1 (#4526's repro, WRONG CODE): the captured array must survive the
// lambda call — `f` borrows `b`'s bytes, it does not own them.
//
// Pre-fix (origin/main 6c05b4cf): n1=4, then `b` is disposed at the lambda's
// scope end, so n2=0 and `b.Count`=0 → 4. Post-fix: 4 + 4 + 4 == 12.
// ============================================================================
BEZEL_TEST(codegen_dispose_stack_depth_wrong_frame_4526,
           captured_byte_array_survives_the_lambda_that_borrows_it) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Array<byte>(4, byte(7)); "
        "    var f := lambda(): int => do "
        "      using var rb := RawBytes.From(b); "
        "      return rb.Length; "
        "    end; "
        "    var n1: int := f(); "
        "    var n2: int := f(); "
        "    return n1 + n2 + b.Count; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 12);
}

// ============================================================================
// Row 2 (THE ELEMENT READ): the same shape, but the observable is a read of
// the caller's array AFTER the lambda has returned. Row 1's oracle is three
// summed counts, which a future partial fix could satisfy while still
// tearing the storage down; this row reads through it.
//
// Pre-fix: `Unhandled exception: System.IndexOutOfRangeException` (exit 70) —
// `b`'s Count is 0 because the lambda disposed it. Post-fix: 7.
// ============================================================================
BEZEL_TEST(codegen_dispose_stack_depth_wrong_frame_4526,
           captured_byte_array_element_readable_after_the_lambda_returns) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Array<byte>(4, byte(7)); "
        "    var f := lambda(): int => do "
        "      using var rb := RawBytes.From(b); "
        "      return rb.Length; "
        "    end; "
        "    var n1: int := f(); "
        "    return int(b[0]); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 7);
}

// ============================================================================
// Row 3 (BOUNDARY — the SAME-frame registration must survive): the identical
// factory call with no lambda between the local and the call, so
// `declScopeDepth_[b]` really does index THIS body's `disposeStack_`. This is
// #998's own shape and the thing the accessor must keep admitting: a fix that
// declined every recorded depth would pass rows 1-2 and turn this into a
// leak, which `runAotChecked`'s LSan leg reports.
//
// Measured working on origin/main and post-fix: 4 + 4 == 8, clean.
// ============================================================================
BEZEL_TEST(codegen_dispose_stack_depth_wrong_frame_4526,
           same_frame_byte_array_factory_arg_still_registers) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Array<byte>(4, byte(7)); "
        "    using var rb := RawBytes.From(b); "
        "    var n: int := rb.Length; "
        "    return n + b.Count; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 8);
}

// ============================================================================
// Row 4 (LEAK-AMPLIFIED x200, STRICT ORACLE): the captured-array shape driven
// 200 times. A one-shot LSan row over one small allocation passes vacuously —
// the allocation can stay reachable from a live stack slot — so the loop is
// what makes the "no leak was traded for the fix" half of row 1's claim real.
//
// The row also re-asserts the VALUE (`total = 2400`, i.e. 200 x 12) rather
// than exit 0 alone: measured on origin/main this program is leak-clean and
// exits 0 while printing 800 (200 x the wrong 4), so an exit-code-only oracle
// would pass on both sides — the exact vacuity this file is meant to avoid.
// Post-fix: 2400, clean, exit 0.
// ============================================================================
BEZEL_TEST(codegen_dispose_stack_depth_wrong_frame_4526,
           captured_byte_array_no_leak_amplified_200x) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Once(): int "
        "    var b := new Array<byte>(4, byte(7)); "
        "    var f := lambda(): int => do "
        "      using var rb := RawBytes.From(b); "
        "      return rb.Length; "
        "    end; "
        "    var n1: int := f(); "
        "    var n2: int := f(); "
        "    return n1 + n2 + b.Count; "
        "  end Once; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..200 do "
        "      total := total + C.Once(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    if total <> 2400 then "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 0);
}
