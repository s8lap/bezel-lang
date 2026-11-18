// Codegen — a +1 `string` and the built-in collections (§28.11, §28.9).
//
// A collection of `string` holds one reference per stored element. A query
// (`Contains`, `IndexOf`, `in`, …) BORROWS its operand (§17.5) and keeps
// nothing; the fill constructor stores the fill once per slot. Each row pins
// that the reference count follows: a fresh operand of a query is released by
// the query, and every filled slot owns its reference. A string taken out by
// `Pop()` / `Dequeue()` is still a borrow to its consumer (#8290 is open):
// the last two rows pin that a string stored through a monomorphized generic
// body at `T = string`, which takes no reference (#8453), is not freed by
// the concrete side's `Pop()` / `Dequeue()` or by the caller of the generic
// store.
//
// Every row loops 200x under `runAotChecked`, whose leak check is the oracle,
// except where a row says it runs ASan without the leak check.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// #8648 (§17.5 / §28.9): a membership or search query borrows its +1 string
// operand — `Contains` / `IndexOf` / `LastIndexOf` / `Remove` / `in` over an
// Array, a Set, a LinkedList, an inline array and a span, whatever the
// element type (`string` or `Object`), the substring `in`, the `string.Join`
// separator — and a Set that does not insert a duplicate does not take it
// either. Each releases it. 200x.
BEZEL_TEST(codegen_str_collection_release, string_producer_collection_query_is_released_8648_spec_17_5) {
    auto r = runAotChecked(
        "module M; "
        "class H "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Mint(): string return \"KM\" + _n.ToString(); end Mint; "
        "  public method Long(): string return \"long-producer-string-no-\" + _n.ToString(); end Long; "
        "  public method MintQ(): string? return \"KQ\" + _n.ToString(); end MintQ; "
        "  public property P: string get => \"KP\" + _n.ToString(); end P; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var h := new H(7); "
        "    var a := new Array<string>(); "
        "    a.Add(\"x\"); "
        "    var hs := new Set<string>(); "
        "    hs.Add(\"x\"); "
        "    var hd := new Set<string>(); "
        "    hd.Add(\"long-producer-string-no-7\"); "
        "    var ll := new LinkedList<string>(); "
        "    ll.AddLast(\"x\"); "
        "    var ia: [2]string := [\"x\", \"y\"]; "
        "    var objs := new Array<Object>(); "
        "    objs.Add(\"x\"); "
        "    var ho := new Set<Object>(); "
        "    ho.Add(\"x\"); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      var n := 0; "
        "      if a.Contains(h.Long()) then n := n + 100; end; "
        "      if h.Long() in a then n := n + 100; end; "
        "      if a.IndexOf(h.Long()) < 0 then n := n + 1; end; "
        "      if a.LastIndexOf(h.Long()) < 0 then n := n + 1; end; "
        "      if a.Remove(h.Long()) then n := n + 100; end; "
        "      if hs.Contains(h.Long()) then n := n + 100; end; "
        "      if h.Long() in hs then n := n + 100; end; "
        "      if hs.Remove(h.Long()) then n := n + 100; end; "
        "      hd.Add(h.Long()); "
        "      if not hd.TryAdd(h.Long()) then n := n + 1; end; "
        "      if ll.Contains(h.Long()) then n := n + 100; end; "
        "      if h.Long() in ll then n := n + 100; end; "
        "      if ll.Remove(h.Long()) then n := n + 100; end; "
        "      if ia.Contains(h.Long()) then n := n + 100; end; "
        "      if h.Long() in ia then n := n + 100; end; "
        "      if objs.Contains(h.Long()) then n := n + 100; end; "
        "      if objs.IndexOf(h.Long()) >= 0 then n := n + 100; end; "
        "      if h.Long() in objs then n := n + 100; end; "
        "      if objs.Remove(h.Long()) then n := n + 100; end; "
        "      if ho.Contains(h.Long()) then n := n + 100; end; "
        "      var sp := a.AsSpan(); "
        "      if sp.Contains(h.Long()) then n := n + 100; end; "
        "      if sp.IndexOf(h.Long()) >= 0 then n := n + 100; end; "
        "      if h.Long() in \"zz\" then n := n + 100; end; "
        "      var j := string.Join(h.Long(), a); "
        "      n := n + j.Length; "
        "      if n = 3 + 1 and hd.Count = 1 then tot := tot + 1; end; "
        "    end while; "
        "    if tot = 200 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8290 / #8453 (stdlib §4.11, §28.11, §28.9, v0.357): a string stored
// through a generic `PushIt<T>` / `EnqIt<T>` at `T = string` (whose body
// takes no reference) and taken out by a CONCRETE `Stack<string>.Pop()` /
// `Queue<string>.Dequeue()` must not be released by the concrete consumer:
// the reference it would release is the caller's own `s`. With `Pop()` /
// `Dequeue()` classified as +1 producers the bind / `.Length` read released
// it and the next iteration read freed memory (heap-use-after-free, the
// review's probes a1_genpush_pop / a2_genenq_deq). Clean under the leak
// check on the series' base. 200x.
BEZEL_TEST(codegen_str_collection_release, generic_push_then_concrete_pop_does_not_free_the_caller_string_8290_spec_28_11) {
    auto r = runAotChecked(
        "module M; "
        "class G "
        "  public static method PushIt<T>(st: Stack<T>, x: T) st.Push(x); end PushIt; "
        "  public static method EnqIt<T>(q: Queue<T>, x: T) q.Enqueue(x); end EnqIt; "
        "end G; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"heap\" + 7.ToString(); "
        "    var stk := new Stack<string>(); "
        "    var q := new Queue<string>(); "
        "    var i := 0; var tot := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      G.PushIt(stk, s); "
        "      var p := stk.Pop(); "
        "      tot := tot + p.Length; "
        "      G.EnqIt(q, s); "
        "      tot := tot + q.Dequeue().Length; "
        "    end while; "
        "    if tot = 200 * (5 + 5) and s = \"heap7\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8290 / #8453 (stdlib §4.11, §28.11, §28.9, v0.357): a concrete
// `Stack<string>.Pop()` passed to a generic `W<T>.Push(x: T)` that stores it
// (no reference taken at `T = string`) must not be released by the caller
// after the call. With `Pop()` classified as a +1 producer the caller
// released it, `W.Pop()` returned freed memory (heap-use-after-free, the
// review's probe b2_wgen_pop). The program leaks the popped string on every
// tree until #8453 is fixed, so the oracle is ASan without the leak check.
// 200x.
BEZEL_TEST(codegen_str_collection_release, concrete_pop_into_generic_store_is_not_freed_8290_spec_28_11) {
    auto r = runAotCheckedLenient(
        "module M; "
        "class W<T> "
        "  private owned _s: Stack<T>; "
        "  public constructor() _s := new Stack<T>(); end constructor; "
        "  public method Push(x: T) _s.Push(x); end Push; "
        "  public method Pop(): T return _s.Pop(); end Pop; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var src := new Stack<string>(); "
        "    var i := 0; var tot := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      src.Push(\"k\" + i.ToString()); "
        "      var ww := new W<string>(); "
        "      ww.Push(src.Pop()); "
        "      var p := ww.Pop(); "
        "      tot := tot + p.Length; "
        "    end while; "
        "    if tot = 9 * 2 + 90 * 3 + 101 * 4 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8654 (§28.11 / §28.9): `new Array<string>(size, fill)` — every slot owns
// a reference to the fill string, a +1 fill is released once the slots hold
// theirs (size 0 included), and a local passed as the fill stays the
// caller's. Before the fix the slots shared ONE reference (a use after free
// under ASan at size 3). 200x.
BEZEL_TEST(codegen_str_collection_release, string_fill_constructor_takes_a_reference_per_slot_8654_spec_28_11) {
    auto r = runAotChecked(
        "module M; "
        "class H "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Mint(): string return \"KM\" + _n.ToString(); end Mint; "
        "  public method Long(): string return \"long-producer-string-no-\" + _n.ToString(); end Long; "
        "  public method MintQ(): string? return \"KQ\" + _n.ToString(); end MintQ; "
        "  public property P: string get => \"KP\" + _n.ToString(); end P; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var h := new H(7); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      var n := 0; "
        "      var a := new Array<string>(3, h.Long()); "
        "      n := n + a.Count + a[2].Length; "
        "      var b := new Array<string>(0, h.Long()); "
        "      n := n + b.Count; "
        "      var t := h.Mint(); "
        "      var c := new Array<string>(2, t); "
        "      c[0] := \"z\"; "
        "      n := n + c[1].Length + t.Length; "
        "      if n = 3 + 25 + 0 + 3 + 3 then tot := tot + 1; end; "
        "    end while; "
        "    if tot = 200 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}
