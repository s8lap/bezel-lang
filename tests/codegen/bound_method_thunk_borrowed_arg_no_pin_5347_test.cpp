// Codegen / refcounting — issue #5347, use-after-free: a bound-method thunk
// takes no borrowed-argument pin.
//
// The bound-method-thunk call path (`emitClosureCall`, LlvmBackend_Calls2.cpp)
// builds its own argument list with direct `lowerExpr` calls and emits the call
// directly — it does NOT route through `buildOrderedArgs` and therefore never
// calls `emitBorrowedStringArgPins`. A borrowed `string` argument passed to
// such a call is UNPINNED: the callee's borrowed parameter receives a bare
// pointer to the caller's string, with no retained reference to keep it alive.
//
// When the callee (a) stores the borrowed parameter into a field whose sole
// reference is that pin, (b) then overwrites that field with a fresh runtime
// string (orphaning the original), and (c) RE-READS the parameter afterwards,
// the parameter dangles: the orphaning store dropped the last live reference
// and freed the allocation, so the read hands a freed pointer back to the
// caller. Under ASan that is a SEGV inside `bz_string_release`.
//
// Spec: §27.6 (bound method references) and §28.3 / §28.9 (borrowed
// argument pins). v0.313.
//
// THE ORACLE. `runAot` — the plain leg pins the program LOGIC (the printed
// length). `runAotChecked` — generated-code ASan + LeakSanitizer — is what
// actually catches the use-after-free; the freed pointer is read back as a
// `string`, so without the fix the plain leg prints garbage or crashes
// nondeterministically rather than failing deterministically on stdout. The
// checked leg is the discriminating oracle: without the fix it reports
// `AddressSanitizer: SEGV ... in bz_string_release`; with the fix it is clean.
//
// MEASURED fail-without (200 iterations, ASan):
//   AddressSanitizer: SEGV on unknown address 0xfffffffffffffffb
//     #2 bz_string_release  src/runtime/bezel_runtime_refcounting.cpp:144
//     #3 main
//   (with the fix: clean run, prints "201")

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// SHAPE: bound-method-thunk orphan, PARAMETER re-read. `Orphan` stores the
// borrowed `string` parameter into `_v` (so the field's only reference is
// shared with the missing borrow pin's +1), then DISPLACES `_v` with a fresh
// runtime string (`x + "x"` — the orphaning store, whose release-old drops
// the last live reference and frees the allocation), then RETURNS the
// parameter `x` — now a dangling pointer. The caller passes the freed string
// to `bz_string_to_utf8z` / `s->len` — a SEGV.
//
// `Orphan` is a method on the SAME class as `Val`, so the bound-method
// reference `var f := d.Orphan` captures `d` as the receiver. The call
// `f(d.Val)` passes `d.Val` (the borrowed string) and returns the dangling
// value; the caller reads its `.Length`.
//
// 200 ITERATIONS: the repro is a 200/200 one-shot. The loop is also what
// makes the original `stringPinArgUnwindCleanup`-based fix fail IR
// verification ("Instruction does not dominate all uses") — the cleanup
// block it creates sits in the entry block while its uses sit in the loop
// body. The shipped fix emits the throw-path release directly in the
// `call.unwind` block `emitPendingCheck` creates, which dominates correctly.
std::string boundMethodThunkOrphanSrc() {
    return "module M5347; "
           "public class BoomO "
           "  private _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "  public property Val: string => _v; "
           "  public method Orphan(x: string): string "
           "    _v := x; "          // step 1: store borrowed param into field
           "    _v := x + \"x\"; "  // step 2: orphan the field's original value
           "    return x; "         // step 3: re-read the (now dangling) param
           "  end Orphan; "
           "end BoomO; "
           "public class Program "
           "  public static method Main(): int "
           "    using var d := new BoomO(\"a1\"); "
           "    var f := d.Orphan; "  // bound-method thunk
           "    for var i in 0..<200 do "
           "      var r := f(d.Val); "
           "      if i = 199 then "
           "        Console.WriteLine(r.Length); "
           "      end if; "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// The bound-method-thunk orphan row. Without the fix: ASan SEGV in
// `bz_string_release` (the orphaning store's release-old frees the string the
// parameter still names). With the fix: clean, prints the correct length.
//
// MEASURED: `a1` grows by one "x" per call (the field accumulates), so after
// 200 iterations the returned value is 201 characters long.
BEZEL_TEST(bound_method_thunk_borrowed_arg_no_pin_5347,
           bound_method_thunk_orphan_pin_spec_27_6_28_3_v0_313) {
    const std::string src = boundMethodThunkOrphanSrc();
    const std::string want = "201\n";

    // plain leg: program logic, no sanitizer
    auto plain = runAot(src);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, want);

    // checked leg: ASan + LeakSanitizer. `ran` false on any UAF or leak
    // report — this is the half that discriminates (the plain leg can read a
    // freed-but-not-yet-reused allocation and print a plausible number).
    auto checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    if (!checked.ran) return;
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, want);
}
