// fix #3453 — a UNIT-TYPED local (or parameter) inside an `async` body or an
// iterator body killed `bezelc` with SIGILL and NO DIAGNOSTIC (spec §4.7 *The
// unit type*, §30.1 *`async` methods*, §29.3 *Iterator methods*, §21.1
// *Variable declarations* — v0.283).
//
// MEASURED on ef339fa, with the issue's repro compiled by the real driver:
//
//     public static async method Work(): Task<int>
//       var u: () := ();
//       return 7;
//     end Work;
//
//   bezelc: rc=132 (SIGILL), stdout empty, stderr empty — no diagnostic of any
//   severity, nothing naming the construct. Under gdb the trap is:
//
//     #0  llvm::StructLayout::StructLayout(llvm::StructType*, DataLayout const&)
//     #1  llvm::DataLayout::getStructLayout(llvm::StructType*) const
//     ...
//     #6  bezel::codegen::Emitter::emitAsyncMethod   (LlvmBackend_Async.cpp:1134)
//
// i.e. the very first `getTypeAllocSize(stateStructTy)` — the frame's `calloc`
// size — over a state-machine frame struct that had been given a **`void`
// member**. `llvm_unreachable` inside LLVM's struct-layout walk is a `ud2` in a
// release build, which is why the failure was a hardware trap and not a message.
//
// ── WHERE THE `void` CAME FROM ───────────────────────────────────────────────
//
// §4.7: "`()` is the type with exactly one value". `lowerTypeRef` answers LLVM
// `void` for exactly that one type and a sized type for every other, and each
// STORAGE position is supposed to substitute the one inert byte
// (`unitPlaceholderTy`) instead — `entryAlloca` does it structurally for the
// local's own alloca, `lowerParamType`/`lowerParamTypeRef` do it at signatures.
// A state-machine frame FIELD is a storage position too, and neither
// `collectSuspendLocals` (locals) nor `buildStateMachineFrame` (captured
// parameters) made the substitution — they called the raw lowering. §30.1 is
// what puts the local there at all: "an async method captures its parameters
// into a heap-allocated state-machine frame that outlives the synchronous call".
//
// ── WHY THESE ROWS, AND WHAT MAKES EACH ABLE TO FAIL ─────────────────────────
//
// Pre-fix EVERY row here fails at `r.ran` — the compiler never produced a
// binary. "It no longer crashes" is therefore trivially satisfied by any change
// that stops the trap, including one that plants a wrong-width field: a `ptr`
// slot for an `i8` alloca is the #461 overflow, which corrupts the ADJACENT
// frame field and shows up only after a real suspension. So every row asserts a
// COMPUTED ANSWER, and T2 exists specifically to make the neighbouring frame
// fields observable across an `await` (§30.2) rather than trusting that a frame
// which merely lays out is a frame that round-trips.
//
// The venues are separate rows because they are separate code paths, each
// measured trapping on ef339fa: an async local, an async `()` PARAMETER (the
// capture-field half, a different loop in a different function), a synchronous
// ITERATOR local (§29.3 — a second frame builder), the §21.1 `const` spelling
// (`ast::LocalConstDecl`, a different AST node), and a `()` local in an async
// body that also boxes a capture cell (§27.5, the pre-boxing-type path).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE ISSUE'S EXACT REPRO. Pre-fix: `bezelc` rc=132, no diagnostic, no
// binary (`r.ran` false). Post-fix the program compiles and the async method's
// own result comes back, so the row pins a VALUE and not merely survival.
BEZEL_TEST(codegen_unit_local_async_frame_field_3453,
           unit_local_in_async_body_compiles_and_returns_spec_4_7_30_1) {
    auto r = runAot(
        "module M3453A; "
        "class C "
        "  public static async method Work(): Task<int> "
        "    var u: () := (); "
        "    return 7; "
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    var v := await Work(); "
        "    Console.WriteLine(v.ToString()); "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\ndone\n"));
}

// ── T2 — THE WIDTH ORACLE. A frame field that merely EXISTS is not enough: the
// per-suspend spill stores `alloca->getAllocatedType()` bytes into it, so a
// field of the wrong width overflows into the next one (#461) and the damage is
// visible only AFTER a real suspension (§30.2). The unit local is declared
// BETWEEN two locals that must survive the `await` unchanged, and all three —
// plus the awaited result and the unit local's own §8.5 rendering — are printed
// afterwards. A `ptr`-width field for the `i8` unit alloca corrupts `s`; a
// zero-width one shifts every later field.
BEZEL_TEST(codegen_unit_local_async_frame_field_3453,
           unit_frame_field_does_not_disturb_neighbours_spec_30_1_30_2) {
    auto r = runAot(
        "module M3453B; "
        "class C "
        "  public static async method Inner(n: int): Task<int> "
        "    return n * 2; "
        "  end Inner; "
        "  public static async method Work(): Task<int> "
        "    var before := 11; "
        "    var u: () := (); "
        "    var s := \"kept\"; "
        "    var got := await Inner(5); "
        "    Console.WriteLine(s); "
        "    Console.WriteLine(before.ToString()); "
        "    Console.WriteLine(got.ToString()); "
        "    Console.WriteLine(u.ToString()); "
        "    return before + got; "
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    var v := await Work(); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("kept\n11\n10\n()\n21\n"));
}

// ── T3 — the CAPTURE-FIELD half. §30.1: "an async method captures its
// parameters into a heap-allocated state-machine frame". A `()` PARAMETER is
// planned by a different loop, in a different function (`buildStateMachineFrame`
// rather than `collectSuspendLocals`), and trapped on ef339fa in its own right.
// The capture field must also agree with `lowerParamType`'s answer for the
// parameter's own slot, because the frame store loads through that type.
BEZEL_TEST(codegen_unit_local_async_frame_field_3453,
           unit_parameter_of_async_method_spec_4_7_30_1) {
    auto r = runAot(
        "module M3453C; "
        "class C "
        "  public static async method Work(u: (), n: int): Task<int> "
        "    Console.WriteLine(u.ToString()); "
        "    return n + 1; "
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    var v := await Work((), 6); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n7\n"));
}

// ── T4 — the SYNCHRONOUS ITERATOR venue (§29.3). An iterator body gets the same
// heap frame from the same builder, so the same `void` field was planned and the
// same trap fired. The loop sums the yielded values, so the row cannot pass on a
// frame whose fields have shifted.
BEZEL_TEST(codegen_unit_local_async_frame_field_3453,
           unit_local_in_iterator_body_spec_4_7_29_3) {
    auto r = runAot(
        "module M3453D; "
        "class C "
        "  public static method Seq(): IEnumerable<int> "
        "    var u: () := (); "
        "    yield return 1; "
        "    Console.WriteLine(u.ToString()); "
        "    yield return 2; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var x in C.Seq() do "
        "      n := n + x; "
        "    end for; "
        "    Console.WriteLine(n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n3\n"));
}

// ── T5 — the §21.1 runtime-`const` spelling. `const u: () := ();` is
// `ast::LocalConstDecl`, a DIFFERENT AST node reaching the frame planner through
// a different arm (the #3375 one). It trapped identically on ef339fa, and a fix
// that only guarded the `var` arm would leave it trapping.
BEZEL_TEST(codegen_unit_local_async_frame_field_3453,
           const_unit_local_in_async_body_spec_21_1_30_1) {
    auto r = runAot(
        "module M3453E; "
        "class C "
        "  public static async method Inner(): Task<int> "
        "    return 4; "
        "  end Inner; "
        "  public static async method Main(): Task<int> "
        "    const u: () := (); "
        "    var v := await Inner(); "
        "    Console.WriteLine(u.ToString()); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n4\n"));
}

// ── T6 — the §27.5 CAPTURED spelling. A lambda-captured local's frame field is
// widened to `ptr` (it holds the capture cell), and the local's PRE-BOXING type
// is remembered for a second field (#3484's fall-back address). That remembered
// type was the raw `void` too, so this shape trapped through a third path. The
// closure is invoked AFTER the suspension, so the row also proves the capture
// survives it.
BEZEL_TEST(codegen_unit_local_async_frame_field_3453,
           captured_unit_local_across_await_spec_4_7_27_5_30_1) {
    auto r = runAot(
        "module M3453F; "
        "class C "
        "  public static async method Inner(): Task<int> "
        "    return 4; "
        "  end Inner; "
        "  public static async method Main(): Task<int> "
        "    var u: () := (); "
        "    var f := lambda() => u.ToString(); "
        "    var v := await Inner(); "
        "    Console.WriteLine(f()); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("()\n4\n"));
}
