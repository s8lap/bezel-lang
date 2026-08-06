// Spec-derived regression test for issue #2902.
//
// §31.1 (Throwing) / §31.4 (Standard exception types) / §28.9 (Runtime-managed
// types) / §28.1 (Where the `owned` modifier can appear) / §22.8 (Field
// initializers), v0.240.
//
// WHAT THE DEFECT IS. A user class deriving from a BUILT-IN exception carries
// that base's §31.4 header — `classStruct` inserts Message@8 / InnerException@16
// / CauseChain@24 / ParamName@32 whenever `hasBuiltinExceptionBase` holds
// (LlvmBackend_Layout.cpp) — and a `base(msg)` / `base(msg, paramName)` chain
// stores into those slots from `Emitter::emitChainCall`
// (LlvmBackend_Setup.cpp). That store was BARE: no retain, no transfer
// decision at all.
//
// §31.4 declares `Message` a `readonly property Message: string` of the
// exception, so the exception holds it; a `string` is runtime-managed (§28.9,
// whose lifecycle "is not part of this specification" — here, reference
// counting). The direct-`new` built-in path has implemented that ownership as
// the #1033 retain/transfer split since then (LlvmBackend_Exprs2.cpp): a `+1`
// producer TRANSFERS its single reference into the slot; a BORROW is RETAINED
// so the slot owns an independent `+1`. The chain site did neither, and the
// two sites disagreeing is why this survived.
//
// WHY IT FAULTS. The argument of a `base(m)` chain is normally the user
// constructor's own `string` PARAMETER — a borrow whose single reference
// belongs to the caller (§28.1: on a `string`, "`owned` is a compile error:
// such values carry no disposal obligation to transfer (§28.9)"). Reading the
// emitted `main` for the repro below: `new SubEx("m" + i.ToString())` builds
// the message as a `+1` temp, calls `bz$App.SubEx$ctor`, and then at
// `new.cont` — the very next block — does `call void @bz_string_release(ptr
// %str.obj)`. That was the only reference. The `Message` slot still points at
// the freed block, so the next `e.Message` read faults.
//
// MEASURED ON THE PRE-FIX COMPILER (f069645), 20 iterations plus a churn
// allocation, generated-code ASan:
//     ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8 in main
// The identical loop built on the BUILT-IN `Exception` directly is clean and
// prints 102 — that control is what localizes the defect to the chain site,
// and it is kept below.
//
// The message must be built at RUNTIME. A string LITERAL is an immortal
// sentinel on which retain and release are both no-ops, which is exactly why
// every literal-message example in the suite hid this defect; the literal cell
// below is kept deliberately, as a control that the fix did not disturb it.
//
// THE `paramName` TWIN. The issue filed it as "inferred from code shape, not
// reproduced". It is REAL, and reproduced here. `ParamName` is unreadable
// through the user-subclass static type (`no member ParamName on type SubAe`,
// because `lowerMemberAccess`'s ParamName arm gates on
// `isBuiltinExceptionTypeRef`), but it IS readable once the instance is caught
// as its built-in base `ArgumentException` — and with a LITERAL message
// isolating the slot from Message@8, the pre-fix compiler gives the same
// `heap-use-after-free`.
//
// ============================================================================
// WHAT THIS FIX DELIBERATELY DOES NOT DO — read before strengthening a cell.
// ============================================================================
//
// Making the slot own a reference does not by itself make it exactly-once:
// NOTHING releases a user exception subclass's inherited Message. `vt$<C>`
// slot 2 is the class's own `bz$<C>$Dispose`, whose core never touches the
// §31.4 header, and `emitBaseDisposeChain` (§28.4 step 3, verbatim: "Chains to
// the base class's `Dispose()`.") walks only USER-class ancestors — a built-in
// exception base has no `TypeSymbol`, so the shared `bz$exDisposeMsg` that a
// directly-constructed `new Exception(msg)` disposes through is never reached
// from a subclass.
//
// So this fix converts a heap-use-after-free into a BOUNDED LEAK — one message
// string per subclass instance; measured 711 bytes / 40 allocations over these
// 20 iterations, exactly what the shape leaked before in its `+1`-producer
// spelling. That is the strictly safer of the two directions and the recorded
// #2874 / #2823 reasoning.
//
// The obvious completion — calling `builtinExceptionDisposeFn_` from
// `emitDisposerCore` — was written and MEASURED, and is BLOCKED by a distinct,
// pre-existing cross-module defect (filed as #2950): an IMPORTED class deriving
// from a built-in exception is laid out WITHOUT the §31.4 header by the
// importer. Measured for one `public class MyEx: Exception` with a single `int`
// field — exporter `%class.MyEx = type { ptr, ptr, ptr, ptr, ptr, i8, i32 }`
// (48 B) versus importer `{ ptr, i8, i32 }` (16 B). Adding the release made the
// exporter-compiled disposer hand the importer's *field value* to
// `bz_string_release`: `SEGV on unknown address 0x2b`, `rdi = 0x13` (the field's
// value, 19), in `bz$TsLib2867T1.MyEx2867$DisposeCore` — i.e. the existing
// `xmod_object_tostring_producer_residual_2867` test. Releasing is only sound
// once the two modules agree on the layout.
//
// CONSEQUENCE FOR THIS FILE'S ORACLES. The cells that pin the fixed defect run
// under `runAotCheckedLenient` — generated-code ASan WITHOUT LSan — because the
// residual leak above is expected and tracked, while a use-after-free (an ASan
// abort, which flips `r.ran` to false) is exactly what must not come back. The
// two cells that were already leak-free — the built-in control and the literal
// sentinel — run under the FULL `runAotChecked`, so a fix that introduced a
// leak where there was none would still be caught here. When #2950 and the
// residual (#2951) land, every lenient cell below should be promoted to
// `runAotChecked`.
//
// EVIDENCE DISCIPLINE. Every message/paramName is built at RUNTIME
// (`"m" + i.ToString()`); every cell is LOOP-AMPLIFIED 20x with a churn
// allocation (a one-shot `Main` is not a reliable memory oracle — a leftover
// pointer in a register makes a leaked block read as merely reachable); and
// "exit 0" is never the whole oracle — each `Main` recomputes a total from the
// values it read and returns non-zero on a mismatch, so a degraded null or
// empty string fails loudly rather than passing quietly. Note that the printed
// total alone would NOT have caught the original defect: the freed block still
// held its bytes, so the pre-fix program printed the right number while
// reading freed memory. The ASan report is the load-bearing oracle.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

std::string out(const bezel::test::JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// The reported shape, and the control that localizes it.
// ---------------------------------------------------------------------------

// #2902 / §31.1 + §28.9 + §22.8 — the issue's own repro. `base(m)` on a
// built-in exception base with `m` a BORROWED ctor parameter and the message
// built at runtime by the caller. PRE-FIX: heap-use-after-free on the
// `e.Message` read.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           base_chain_borrow_message_no_use_after_free_31_1) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class SubEx: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end SubEx; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: SubEx := new SubEx(\"m\" + i.ToString()); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + e.Message.Length + churn.Length; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2902 / §31.4 — the CONTROL that localized the defect: the identical loop
// built on the BUILT-IN `Exception` directly. This path was already correct (it
// is the #1033 site the fix mirrors) and is leak-free, so it runs under the
// FULL checker: a fix that "repaired" the subclass by disturbing the shared
// built-in construction or disposal would be caught here.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           builtin_exception_direct_new_control_unchanged_31_4) {
    auto r = runAotChecked(
        "module App; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: Exception := new Exception(\"m\" + i.ToString()); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + e.Message.Length + churn.Length; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// ---------------------------------------------------------------------------
// The `paramName` twin — filed as INFERRED, measured here as REAL.
// ---------------------------------------------------------------------------

// #2902 / §31.4 — `ArgumentException`'s `(message, paramName)` chain
// (§31.4: "public pure constructor(message: string, paramName: string);").
// The message is a LITERAL — an immortal sentinel, so it cannot be the faulting
// block — and only the runtime-built `ParamName` is read, isolating
// ParamName@kExcParamOffset from Message@kExcMsgOffset. `ParamName` is read
// through the built-in base's static type, the only spelling that resolves it.
// PRE-FIX: heap-use-after-free.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           base_chain_paramname_isolated_no_use_after_free_31_4) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class SubAe: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); "
        "  end constructor; "
        "end SubAe; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    try "
        "      throw new SubAe(\"literal\", \"p\" + i.ToString()); "
        "    catch "
        "      var e: ArgumentException => do "
        "        var churn: string := \"c\" + i.ToString(); "
        "        t := t + e.ParamName!.Length + churn.Length; "
        "      end "
        "    end; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2902 / §31.4 — both header string slots live at once, message AND paramName
// both built at runtime. Pins that the two stores' retain decisions are
// INDEPENDENT (each consults its own argument expression), not one decision
// computed once and applied twice.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           base_chain_message_and_paramname_both_runtime_31_4) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class SubAe2: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); "
        "  end constructor; "
        "end SubAe2; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    try "
        "      throw new SubAe2(\"m\" + i.ToString(), \"p\" + i.ToString()); "
        "    catch "
        "      var e: ArgumentException => do "
        "        var churn: string := \"c\" + i.ToString(); "
        "        t := t + e.ParamName!.Length + e.Message.Length "
        "               + churn.Length; "
        "      end "
        "    end; "
        "  end while; "
        "  if t <> 153 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// ---------------------------------------------------------------------------
// Structural axes — the shapes a narrower fix would miss.
// ---------------------------------------------------------------------------

// #2902 / §12 + §31.4 — a MULTI-LEVEL user hierarchy (`ExB: ExA: Exception`).
// `hasBuiltinExceptionBase` is transitive, so `ExB` also carries the header and
// its OWN `base(m)` chain stores into it; a fix keyed on "the direct AST base is
// a built-in" at the wrong level would miss this.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           multi_level_subclass_hierarchy_no_use_after_free_12) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class ExA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExA; "
        "class ExB: ExA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExB; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: ExB := new ExB(\"m\" + i.ToString()); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + e.Message.Length + churn.Length; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2902 / §31.1 — the THROWN-and-CAUGHT path rather than the `using var`
// scope-end one. §31.1 makes a caught exception a borrow the RUNTIME owns and
// disposes when the arm completes, so the message is read through a different
// site with a different lifetime; the store must own its reference on both.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           thrown_and_caught_subclass_no_use_after_free_31_1) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class SubEx3: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end SubEx3; "
        "class C "
        "  public static method Boom(n: int): int "
        "    throw new SubEx3(\"m\" + n.ToString()); "
        "  end Boom; "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      try "
        "        t := t + Boom(i); "
        "      catch "
        "        var e: SubEx3 => do "
        "          var churn: string := \"c\" + i.ToString(); "
        "          t := t + e.Message.Length + churn.Length; "
        "        end "
        "      end; "
        "    end while; "
        "    if t <> 102 then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2902 / §31.4 — the SIBLING branch of the one the fix edits. `emitChainCall`
// discriminates `base(msg, X)` on X's static type: a `string` X is
// `ArgumentException`'s paramName (the slot the fix now retains), anything else
// is §31.4's `(message, cause)` overload, whose cause is "borrow[ed] ... for the
// duration of the call" and snapshotted into `CauseChain`. The fix sits inside
// the `argIsString` arm, so this pins that the else-arm still routes and still
// renders — a mis-edit that retained the *cause* operand, or that collapsed the
// discrimination, changes this total.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           message_and_cause_overload_sibling_branch_undisturbed_31_4) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class SubEx7: Exception "
        "  public constructor(m: string, c: Exception) base(m, c); "
        "  end constructor; "
        "end SubEx7; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    try "
        "      try "
        "        throw new Exception(\"inner\" + i.ToString()); "
        "      catch "
        "        var ie: Exception => "
        "          throw new SubEx7(\"outer\" + i.ToString(), ie) "
        "      end; "
        "    catch "
        "      var e: Exception => do "
        "        var churn: string := \"c\" + i.ToString(); "
        "        t := t + e.Message.Length + e.CauseChain.Length "
        "               + churn.Length; "
        "      end "
        "    end; "
        "  end while; "
        "  if t <> 202 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2902 / §28.9 — the TRANSFER arm: the `base(...)` argument is itself a `+1`
// PRODUCER built inside the constructor, so `isPlusOneStringProducer` holds and
// the fix must NOT retain (the reference is transferred instead). Honest note
// on this cell's strength: under the residual described in the header, a
// wrongly-added retain here is currently UNOBSERVABLE (both spellings leak the
// same block either way), so what this pins today is that the transfer arm
// still lowers, does not fault, and renders the right text. It becomes a true
// exactly-once oracle the moment the residual lands — promote it to
// `runAotChecked` then, together with the rest.
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           base_chain_producer_message_transfer_arm_28_9) {
    auto r = runAotCheckedLenient(
        "module App; "
        "class SubEx8: Exception "
        "  public constructor(n: int) base(\"m\" + n.ToString()); "
        "  end constructor; "
        "end SubEx8; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: SubEx8 := new SubEx8(i); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + e.Message.Length + churn.Length; "
        "  end while; "
        "  if t <> 102 then return 1; end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
}

// #2902 / §28.9 — the LITERAL-message control, kept because it is the shape
// that hid the defect: a literal is an immortal sentinel, so the added retain is
// a no-op on it and the program stays leak-free — hence the FULL checker here.
// The message text is re-read and PRINTED after the loop, so a fix that nulled
// or over-released the slot would print the wrong text (or fault) rather than
// passing on "exit 0".
BEZEL_TEST(codegen_exception_base_chain_message_owned_2902,
           literal_message_sentinel_unaffected_28_9) {
    auto r = runAotChecked(
        "module App; "
        "class SubEx6: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end SubEx6; "
        "class C public static method Main(): int "
        "  var t: int := 0; "
        "  var i: int := 0; "
        "  while i < 20 do "
        "    i := i + 1; "
        "    using var e: SubEx6 := new SubEx6(\"literal\"); "
        "    var churn: string := \"c\" + i.ToString(); "
        "    t := t + e.Message.Length + churn.Length; "
        "  end while; "
        "  Console.WriteLine(t); "
        "  using var last: SubEx6 := new SubEx6(\"literal\"); "
        "  Console.WriteLine(last.Message); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 0);
    BEZEL_EXPECT_EQ(out(r), std::string("191\nliteral\n"));
}
