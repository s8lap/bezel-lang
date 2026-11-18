// Codegen — a +1 `string` and the built-in collections (§28.11, §28.9).
//
// A collection of `string` holds one reference per stored element. A query
// (`Contains`, `IndexOf`, `in`, …) BORROWS its operand (§17.5) and keeps
// nothing; the fill constructor stores the fill once per slot. Each row pins
// that the reference count follows: a fresh operand of a query is released by
// the query, and every filled slot owns its reference.
//
// Every row loops 200x under `runAotChecked`, whose leak check is the oracle.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

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
