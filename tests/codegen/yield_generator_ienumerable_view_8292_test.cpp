// Codegen — a user class whose `GetEnumerator` is a YIELD GENERATOR is
// enumerable through an `IEnumerable<T>` view (issue #8292, spec §11.2 / §29.2
// / §29.11, stdlib §5, v0.355).
//
// THE DEFECT (measured on the pre-fix code, staging tip 3474b85075).
//   `class MyC: IReadOnlyCollection<string>` with
//       public method GetEnumerator(): IEnumerator<string>
//         for var s in f do yield return s + "!"; end for;
//       end GetEnumerator;
//   and then `var c: IEnumerable<string> := m;` followed by ANY consumption of
//   `c` — `c.Where(..).ToArray()`, `c.ToArray()`, or a plain `for var s in c`
//   — SEGFAULTED on the zero page:
//
//     AddressSanitizer: SEGV on unknown address 0x000000000000
//     Hint: pc points to the zero page.
//     #0 0x000000000000  (<unknown module>)
//
//   The `Where`/`ToArray` chain is INCIDENTAL: the crash is in the
//   ENUMERATION of the class through the `IEnumerable<T>` view, and the
//   trigger is that `GetEnumerator` is a yield generator.
//
// THE MECHANISM (src/codegen/LlvmBackend_Assign.cpp, `wrapUserClassAsIEnumerable`).
//   §11.2/§12.1 *member matching* requires a user's
//   `GetEnumerator(): IEnumerator<T>` to match the interface's member EXACTLY
//   under substitution, so the signature can never name the concrete
//   enumerator class. The wrapper therefore DISCOVERS that class — by the
//   structural "MoveNext + Current" scan over the compiled TypeSymbols. A
//   yield generator's enumerator is a COMPILER-GENERATED state machine
//   (LlvmBackend_Iterators.cpp's `emitIteratorMethod`): it has no user
//   TypeSymbol at all, so the scan cannot see it, the wrapper DECLINED, and
//   the RAW user-class instance was handed to the `IEnumerable<T>` consumer.
//   That consumer dispatches vtable slot 4 (`GetEnumerator`) on it — and a
//   user class's own class vtable has no slot 4 for it — so the call went to
//   a NULL function pointer and the program jumped to 0x0.
//
// THE FIX. Make the enumerator discoverable by the signal that actually
//   identifies it: a `yield`-bearing `GetEnumerator` returns an object in
//   §29.11's ITERATOR-VTABLE FORMAT by construction (its word 0 is the
//   5-slot iterator vtable — [0]=td (self-referencing), [1]=Dispose,
//   [2]=MoveNext, [3]=Current, [4]=GetEnumerator), so the wrapper is minted
//   with the `$ad` family, which drives the inner enumerator through its OWN
//   vtable and needs no enumerator class at all.
//
// THE CONTROL ROW (the hand-written enumerator class) is the twin that
//   matters: the IDENTICAL class shape, the IDENTICAL
//   `GetEnumerator(): IEnumerator<string>` signature, the IDENTICAL interface
//   view, and the only difference is that its enumerator is a HAND-WRITTEN
//   class with `MoveNext` + `Current` — so the structural scan finds it, the
//   wrapper was ALREADY minted, and the program already worked. That row
//   must print the same bytes before and after the fix.
//
// ORACLE NOTE (the LAZY rows). The LAZY chain over a string-yielding
//   generator leaks its yielded strings under LeakSanitizer — issue #8112's
//   lazy-LINQ-chain leak, a PRE-EXISTING defect of the lazy chain's element
//   ownership, NOT of this fix. MEASURED: the same lazy chain over the same
//   generator WITHOUT any interface view (so without this fix at all —
//   `g.Seq().Where(..).ToArray()`) leaks byte-for-byte identically on the
//   pre-fix compiler (70 bytes in 4 allocations, the same two allocation
//   sites). The leak is therefore orthogonal to #8292 and out of scope here
//   (the brief for this lane names #8112 explicitly as NOT in scope), and the
//   LAZY rows below use the plain oracle (no leak check) so they measure what
//   they are about — the SEGV and the element order. Every other row uses the
//   LSan-checked oracle.
//
// Spec: §11.2 (interface declaration — member matching), §29.2 (the
// iterator-format wrapper), §29.11 (the enumerator protocol; each
// `GetEnumerator()` call produces an independent enumerator), §21.4 (for-in
// over an `IEnumerable<T>` source), stdlib §5 (the lazy `Where`/`ToArray`
// surface over an `IEnumerable<T>`). Spec revision v0.355.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The class under test: a user class implementing `iface` whose
// `GetEnumerator` is a YIELD GENERATOR. `GetEnumerator` returns the builtin
// `IEnumerator<T>` — the ONLY spelling §11.2/§12.1's member matching admits
// for an `IEnumerable<T>` implementation — so the concrete enumerator class
// can never be named in the signature.
std::string yieldGenClass(const std::string& clsName,
                          const std::string& iface,
                          const std::string& elemTy,
                          const std::string& addArg,
                          const std::string& yieldExpr) {
    return
        "class " + clsName + ": " + iface + "<" + elemTy + "> "
        "  private owned f: Array<" + elemTy + ">; "
        "  public constructor() f := new Array<" + elemTy + ">(); end constructor; "
        "  public property Count: int => f.Count; "
        "  public method Add(item: " + elemTy + ") f.Add(" + addArg + "); end Add; "
        "  public method GetEnumerator(): IEnumerator<" + elemTy + "> "
        "    for var s in f do yield return " + yieldExpr + "; end for; "
        "  end GetEnumerator; "
        "end " + clsName + "; ";
}

// The CONTROL's twin: the same class, the same `GetEnumerator(): IEnumerator<T>`
// signature, the same interface view — but the enumerator is a HAND-WRITTEN
// class exposing `MoveNext` + `Current` (so the structural scan finds it).
std::string handWrittenClass() {
    return
        "class MyE: IReadOnlyCollection<string> "
        "  private owned f: Array<string>; "
        "  public constructor() f := new Array<string>(); end constructor; "
        "  public property Count: int => f.Count; "
        "  public method Add(item: string) f.Add(item); end Add; "
        "  public method GetEnumerator(): IEnumerator<string> "
        "    return new MyEnum(f); "
        "  end GetEnumerator; "
        "end MyE; "
        "class MyEnum: IEnumerator<string> "
        "  private g: Array<string>; "
        "  private i: int := 0; "
        "  public constructor(g: Array<string>) this.g := g; end constructor; "
        "  public method MoveNext(): bool "
        "    if this.i < this.g.Count then this.i := this.i + 1; return true; end if; "
        "    return false; "
        "  end MoveNext; "
        "  public property Current: string get => this.g[this.i - 1]; end Current; "
        "end MyEnum; ";
}

// A program that binds a class instance to an `IEnumerable<T>` VIEW and then
// consumes the view with `body`.
std::string viewProgram(const std::string& cls,
                         const std::string& clsName,
                         const std::string& elemTy,
                         const std::string& adds,
                         const std::string& body) {
    return
        "module M; " + cls +
        "class C public static method Main(): int "
        "  using var m := new " + clsName + "(); " + adds +
        "  var c: IEnumerable<" + elemTy + "> := m; " + body +
        "  return 0; "
        "end Main; end C;";
}

// The strict oracle: every allocation freed exactly once (a leak / double-free
// / use-after-free flips `ran` to false -> exit code -1, distinct from any
// legal exit value).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -1;
}

}  // namespace

// §29.2 / §29.11 / stdlib §5 (#8292): THE ISSUE'S EXACT PROGRAM — a
// yield-generator `GetEnumerator` on a class implementing
// `IReadOnlyCollection<string>`, viewed as `IEnumerable<string>` and consumed
// by the lazy chain `Where(..).ToArray()`. Prints "2 a!".
// Before the fix: the program SEGV'd on the zero page (a call through the
// null vtable slot 4 of the raw user class) — `ran` false, stdout empty.
// Oracle: the plain AOT oracle (no leak check) — see the ORACLE NOTE at the
// top of this file: the lazy chain over a string-yielding generator leaks
// under LSan for a PRE-EXISTING reason (#8112), measured identically without
// this fix.
BEZEL_TEST(codegen_yield_generator_ienumerable_view_8292,
           issue_shape_where_toarray_over_ienumerable_view_spec_29_2) {
    std::string src = viewProgram(
        yieldGenClass("MyC", "IReadOnlyCollection", "string", "item",
                      "s + \"!\""),
        "MyC",
        "string",
        "m.Add(\"a\"); m.Add(\"b\"); ",
        "var q2 := c.Where(lambda(s) => s.Length > 0); "
        "var arr := q2.ToArray(); "
        "Console.WriteLine($\"{arr.Count} {arr[0]}\"); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2 a!\n");
}

// §21.4 / §29.11 (#8292): the SAME view consumed by a plain `for` loop — the
// crash is in the ENUMERATION, not in the lazy chain, so the `Where`/`ToArray`
// spelling is not required to reproduce it. Prints "a!", "b!", "ok".
// Before the fix: the program SEGV'd on the zero page.
BEZEL_TEST(codegen_yield_generator_ienumerable_view_8292,
           plain_forin_over_ienumerable_view_spec_21_4) {
    std::string src = viewProgram(
        yieldGenClass("MyC", "IReadOnlyCollection", "string", "item",
                      "s + \"!\""),
        "MyC",
        "string",
        "m.Add(\"a\"); m.Add(\"b\"); ",
        "for var s in c do Console.WriteLine(s); end for; "
        "Console.WriteLine(\"ok\"); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
    auto r = runAot(src);
    BEZEL_EXPECT_EQ(r.stdoutText, "a!\nb!\nok\n");
}

// §11.2 / §29.2 (#8292): the SAME defect, the OTHER spelling — a class that
// implements `IEnumerable<string>` DIRECTLY (not through
// `IReadOnlyCollection<string>`), with the same yield-generator
// `GetEnumerator`. Prints "a!", "b!", "ok" — IDENTICALLY to the
// `IReadOnlyCollection` row.
// Before the fix: the program SEGV'd on the zero page.
BEZEL_TEST(codegen_yield_generator_ienumerable_view_8292,
           direct_ienumerable_implementation_spec_11_2) {
    std::string src = viewProgram(
        yieldGenClass("MyD", "IEnumerable", "string", "item", "s + \"!\""),
        "MyD",
        "string",
        "m.Add(\"a\"); m.Add(\"b\"); ",
        "for var s in c do Console.WriteLine(s); end for; "
        "Console.WriteLine(\"ok\"); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
    auto r = runAot(src);
    BEZEL_EXPECT_EQ(r.stdoutText, "a!\nb!\nok\n");
}

// §29.2 / §29.11 (#8292): THE CONTROL — the hand-written enumerator class.
// The IDENTICAL class shape, the IDENTICAL `GetEnumerator(): IEnumerator<string>`
// signature and the IDENTICAL interface view, with a HAND-WRITTEN enumerator
// (MoveNext + Current) instead of a yield generator. The structural scan finds
// that class, so the wrapper was ALREADY minted before this fix: this row must
// keep printing the same bytes ("a", "b", "ok") before and after.
BEZEL_TEST(codegen_yield_generator_ienumerable_view_8292,
           control_handwritten_enumerator_class_spec_29_2) {
    std::string src = viewProgram(
        handWrittenClass(),
        "MyE",
        "string",
        "m.Add(\"a\"); m.Add(\"b\"); ",
        "for var s in c do Console.WriteLine(s); end for; "
        "Console.WriteLine(\"ok\"); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
    auto r = runAot(src);
    BEZEL_EXPECT_EQ(r.stdoutText, "a\nb\nok\n");
}

// stdlib §5 / §29.11 (#8292): the LAZY chain over the interface view —
// `Where` then `Select` then `ToArray`, in element order. The element type is
// `int` here so the row is LSan-clean (the string-element lazy chain leaks
// under LSan for the PRE-EXISTING #8112 reason — see the ORACLE NOTE at the
// top of this file); the enumeration path under test is the same one.
// Prints "2 20 30" (1+1=2 then *10; 2+1=3 then *10).
// Before the fix: the program SEGV'd on the zero page.
BEZEL_TEST(codegen_yield_generator_ienumerable_view_8292,
           lazy_chain_where_select_toarray_over_view_spec_5) {
    std::string src = viewProgram(
        yieldGenClass("MyC", "IReadOnlyCollection", "int", "item", "s + 1"),
        "MyC",
        "int",
        "m.Add(1); m.Add(2); ",
        "var arr := c.Where(lambda(s) => s > 0).Select(lambda(s) => s * 10).ToArray(); "
        "Console.WriteLine($\"{arr.Count} {arr[0]} {arr[1]}\"); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
    auto r = runAot(src);
    BEZEL_EXPECT_EQ(r.stdoutText, "2 20 30\n");
}

// §29.3 / §29.11 (#8292): the yield `GetEnumerator` called DIRECTLY on the
// CONCRETE class (no interface view) — the generator drives its own frame.
// This path never went through the §29.2 wrapper, so it must be UNCHANGED by
// the fix. Prints "a!", "b!", "ok".
BEZEL_TEST(codegen_yield_generator_ienumerable_view_8292,
           direct_getenumerator_call_on_concrete_class_spec_29_3) {
    std::string src = viewProgram(
        yieldGenClass("MyC", "IReadOnlyCollection", "string", "item",
                      "s + \"!\""),
        "MyC",
        "string",
        "m.Add(\"a\"); m.Add(\"b\"); ",
        "using var e := m.GetEnumerator(); "
        "while e.MoveNext() do Console.WriteLine(e.Current); end while; "
        "Console.WriteLine(\"ok\"); ");
    BEZEL_EXPECT_EQ(runChecked(src), 0);
    auto r = runAot(src);
    BEZEL_EXPECT_EQ(r.stdoutText, "a!\nb!\nok\n");
}
