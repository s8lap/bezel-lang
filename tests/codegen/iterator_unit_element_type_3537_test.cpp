// fix #3537 — DECLARING an iterator whose ELEMENT TYPE is the unit type killed
// `bezelc` with SIGILL and NO DIAGNOSTIC of any severity (spec §4.7 *The unit
// type*, §29.2 *Desugaring of `for`*, §29.3 *Iterator methods*, §30.7 *Async
// iterators* — v0.283).
//
// MEASURED on b43196a, with the issue's repro compiled by the real driver:
//
//     public static method Seq(): IEnumerable<()>
//       yield return ();
//     end Seq;
//
//   bezelc: rc=132 (SIGILL, core dumped), stdout empty, stderr empty. No value
//   of the iterator is needed and no consumer is needed — the DECLARATION alone
//   is fatal. `IAsyncEnumerable<()>` is rc=132 identically. Under gdb:
//
//     #0  llvm::StructLayout::StructLayout(llvm::StructType*, DataLayout const&)
//     #1  llvm::DataLayout::getStructLayout(llvm::StructType*) const
//     #2..#5  DataLayout::getTypeSizeInBits / StoreSize / AllocSize  (inlined)
//     #6  bezel::codegen::Emitter::emitIteratorMethod
//
// `llvm_unreachable` inside LLVM's struct-layout walk is a `ud2` in a release
// libLLVM, which is why the failure was a hardware trap rather than a message.
//
// ── WHERE THE `void` CAME FROM ───────────────────────────────────────────────
//
// §4.7: "`()` is the type with exactly one value". `lowerTypeRef` answers LLVM
// `void` for exactly that one type; every STORAGE position substitutes the one
// inert byte instead (`unitStorageTy`). The ITERATOR ELEMENT TYPE is such a
// position — the same lowered type becomes `buildStateMachineFrame`'s
// `current_val` FIELD, `Current`'s (vtable slot 3) return type, the operand of
// every load/store against that field, and the callee type the CONSUMER builds
// for slot 3 — and it was the raw lowering at all four faces. #3453 fixed the
// frame's LOCAL and CAPTURE slots; this is the element beside them.
//
// ── WHY FOUR SITES AND NOT THE ONE THE ISSUE PROPOSED ────────────────────────
//
// Both intermediate states were measured while fixing this, and each is a
// reason a row below exists:
//
//   * Substituting only inside `buildStateMachineFrame` over `extraTrailing`
//     (the issue's suggestion) fixes the struct layout and leaves
//     `CreateStore(Constant::getNullValue(elemTy), …)` beside it — the trap
//     moves, it does not go away.
//   * Substituting only on the PRODUCER side compiles the declaration and then
//     kills the consumer: `for var x in Seq() do … end;` was rc=132 in
//     `DataLayout::getABITypeAlign` under `IRBuilderBase::CreateAlignedStore`,
//     from `lowerIEnumerableBuiltinFor` building a `void (ptr)` callee type for
//     a slot 3 the producer emits as `i8 (ptr)`. T2/T3 are that row.
//
// ── WHY THESE ROWS, AND WHAT MAKES EACH ABLE TO FAIL ─────────────────────────
//
// Pre-fix T1–T5 all fail at `r.ran` — no binary was produced. "It no longer
// crashes" is therefore satisfied by any change that stops the trap, INCLUDING
// one that plants a wrong-width `current_val` field: that lays out fine and
// corrupts its NEIGHBOUR in the frame, which is observable only after a real
// suspension. So no row stops at "it compiled":
//
//   T1  the issue's repro (a), declaration only — plus a value, so the row
//       cannot pass on a compiler that emits a broken but well-formed module.
//   T2  the ELEMENT COUNT of a real enumeration, and the neighbouring frame
//       fields (a captured parameter and two body locals living across two
//       yields) read back afterwards. This is the width oracle.
//   T3  the §30.7 twin — an `IAsyncEnumerable<()>` driven by `await for`, whose
//       frame crosses a genuine thread hop.
//   T4  the FOR-LOOP-VARIABLE frame slot (`collectSuspendLocals`' for-loop arm,
//       LlvmBackend_Iterators.cpp). #3453 added that guard and its own commit
//       recorded it as UNREACHABLE, because every source of a `()` element died
//       at the trailing field first. This fix is what makes it reachable, so it
//       is EXERCISED here rather than assumed: a `()`-typed loop variable that
//       lives across a `yield` inside the enclosing iterator.
//   T5  BOTH-SIDES CONTROL. T1–T4 fail 100% without the fix, which is right for
//       a crash fix but leaves the file unable to tell "the fix works" from
//       "the file stopped building". T5 is the identical shape with an `int`
//       element and passes on both sides by construction.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE ISSUE'S EXACT REPRO (a). Pre-fix: rc=132, no diagnostic, no
// binary. The `Main` body deliberately never touches `Seq`: the defect is in
// emitting the DECLARATION, and the row would still pass on a compiler that got
// the consumer wrong, which is what T2 is for.
BEZEL_TEST(codegen_iterator_unit_element_type_3537,
           declaring_sync_iterator_with_unit_element_spec_4_7_29_3) {
    auto r = runAot(
        "module M3537A; "
        "class C "
        "  public static method Seq(): IEnumerable<()> "
        "    yield return (); "
        "  end Seq; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"decl only\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("decl only\n"));
}

// ── T2 — THE WIDTH ORACLE, and the answer to "does it enumerate correctly?".
//
// Three things are asserted that survival alone does not give:
//   * the enumeration yields exactly THREE elements (a `current_val` the
//     consumer mis-reads, or a `MoveNext` that mis-sequences, changes this);
//   * `tag` — a CAPTURED PARAMETER, which lives in a frame field BEFORE
//     `current_val` — still reads 5 after two suspensions;
//   * `a` and `b` — body locals in the fields immediately before the trailing
//     group — still read 6 and "kept" after two suspensions.
// A `current_val` field of the wrong width overflows into the field beside it
// (#461), so it is precisely `a`/`b`/`tag` that a mis-sized element corrupts.
BEZEL_TEST(codegen_iterator_unit_element_type_3537,
           sync_unit_iterator_element_count_and_frame_neighbours_spec_29_3) {
    auto r = runAot(
        "module M3537B; "
        "class C "
        "  public static method Seq(tag: int): IEnumerable<()> "
        "    var a: int := tag + 1; "
        "    var b: string := \"kept\"; "
        "    yield return (); "
        "    yield return (); "
        "    yield return (); "
        "    Console.WriteLine(\"tag=\" + tag.ToString() "
        "                      + \" a=\" + a.ToString() + \" b=\" + b); "
        "  end Seq; "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    for var x in C.Seq(5) do "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine(\"count=\" + n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("tag=5 a=6 b=kept\ncount=3\n"));
}

// ── T3 — THE §30.7 TWIN. The issue's repro (b) was rc=132 on the declaration
// alone, exactly like (a); this row goes further and DRIVES the async
// enumeration through `await for`, so the `()` element round-trips a real
// suspension and the async frame's own neighbouring fields (`seen`, the
// captured `tag`) are read back on the far side.
BEZEL_TEST(codegen_iterator_unit_element_type_3537,
           async_unit_iterator_enumerates_and_keeps_frame_spec_4_7_30_7) {
    auto r = runAot(
        "module M3537C; "
        "class C "
        "  public static async method Seq(tag: int): IAsyncEnumerable<()> "
        "    var seen: string := \"s\" + tag.ToString(); "
        "    yield return (); "
        "    yield return (); "
        "    Console.WriteLine(\"inner \" + seen + \" tag=\" + tag.ToString()); "
        "  end Seq; "
        "  public static async method Run(): Task<int> "
        "    var n: int := 0; "
        "    await for var x in C.Seq(4) do "
        "      n := n + 10; "
        "    end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    t.Wait(); "
        "    Console.WriteLine(\"total=\" + t.Result.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("inner s4 tag=4\ntotal=20\n"));
}

// ── T4 — THE NEWLY-REACHABLE FOR-LOOP-VARIABLE FRAME SLOT.
//
// `collectSuspendLocals`' for-loop arm derives the loop variable's frame-slot
// type from the ITERABLE's first generic argument, not from `frameSlotTy`, so
// #3453 gave it a guard of its own — and that commit recorded the guard as
// unreachable, because an `IEnumerable<()>` died at the trailing field before
// any loop over one could be lowered. This fix makes it reachable, so it is
// measured here rather than assumed dead-or-alive.
//
// `Outer` loops over a `()`-element iterator AND yields inside the loop, so the
// loop variable `u` must round-trip the enclosing iterator's own frame across
// the suspension — which is exactly the field that arm reserves. The `int`
// elements `Outer` yields make the traversal observable: 1 + 2 + 3 = 6, with
// the running `n` proving the loop resumed in the right iteration each time.
BEZEL_TEST(codegen_iterator_unit_element_type_3537,
           unit_for_loop_variable_across_yield_in_iterator_spec_29_3) {
    auto r = runAot(
        "module M3537D; "
        "class C "
        "  public static method Inner(): IEnumerable<()> "
        "    yield return (); "
        "    yield return (); "
        "    yield return (); "
        "  end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    var n: int := 0; "
        "    for var u in C.Inner() do "
        "      n := n + 1; "
        "      yield return n; "
        "    end; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    var cnt: int := 0; "
        "    for var v in C.Outer() do "
        "      sum := sum + v; "
        "      cnt := cnt + 1; "
        "    end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString() "
        "                      + \" cnt=\" + cnt.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("sum=6 cnt=3\n"));
}

// ── T5 — BOTH-SIDES CONTROL. The same producer/consumer shapes as T2 and T4
// with an `int` element, touching no unit-type code path. It passes on BOTH
// sides of the fix by construction, and pins that routing the element type
// through `unitStorageTy` leaves the ordinary element alone — `unitStorageTy`
// is the identity on every non-`void` type, and this row is what says so about
// the emitted program rather than about the helper.
BEZEL_TEST(codegen_iterator_unit_element_type_3537,
           control_int_element_iterator_unaffected_spec_29_3) {
    auto r = runAot(
        "module M3537E; "
        "class C "
        "  public static method Seq(tag: int): IEnumerable<int> "
        "    var a: int := tag + 1; "
        "    var b: string := \"kept\"; "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "    Console.WriteLine(\"tag=\" + tag.ToString() "
        "                      + \" a=\" + a.ToString() + \" b=\" + b); "
        "  end Seq; "
        "  public static method Inner(): IEnumerable<int> "
        "    yield return 7; "
        "    yield return 8; "
        "  end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    var n: int := 0; "
        "    for var u in C.Inner() do "
        "      n := n + u; "
        "      yield return n; "
        "    end; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var s: int := 0; "
        "    for var x in C.Seq(5) do "
        "      s := s + x; "
        "    end; "
        "    var o: int := 0; "
        "    for var v in C.Outer() do "
        "      o := o + v; "
        "    end; "
        "    Console.WriteLine(\"s=\" + s.ToString() + \" o=\" + o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("tag=5 a=6 b=kept\ns=6 o=22\n"));
}
