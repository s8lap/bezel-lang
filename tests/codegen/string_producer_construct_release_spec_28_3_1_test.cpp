// Codegen — a +1 `string` owned by a whole CONSTRUCT: a `for` iterable
// (§21.4), across `yield` / `await` (§29.3, §30.2), iterator abandonment
// (§29.7) and `yield break` (§29.3).
//
// §28.9 leaves a string's lifetime to the runtime "without user
// involvement"; the loop is the only holder of an iterable that is a fresh
// string, so the loop releases it — on every exit, and from the iterator's
// `Dispose` when the iterator is abandoned while suspended inside the loop.
// (The `case`-scrutinee spelling of the same boundary, #7643, is parked
// behind #8453 and has no row here.)
//
// Every row loops under `runAotChecked`, whose leak check is the oracle.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// #8653 (§29.3 / §21.4): a `for` over a string whose body yields. The loop's
// index and the string it walks must survive the suspend; before the fix the
// module did not verify ("Instruction does not dominate all uses"). 200x.
BEZEL_TEST(codegen_str_construct_release, string_for_keeps_loop_state_across_yield_8653_spec_29_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method W(s: string): IEnumerable<int> "
        "    for var c in s do "
        "      yield return 1; "
        "    end; "
        "    yield return 100; "
        "  end W; "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var s := \"abc\" + 7.ToString(); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      for var v in C.W(s) do tot := tot + v; end; "
        "    end while; "
        "    if tot = 200 * 104 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8653 (§30.2 / §21.4): the `await` twin — the loop resumed on a fresh stack
// with its index reset, so the count came back wrong. 50x.
BEZEL_TEST(codegen_str_construct_release, string_for_keeps_loop_state_across_await_8653_spec_30_2) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method W(s: string): Task<int> "
        "    var n := 0; "
        "    for var c in s do "
        "      await Task.Delay(0); "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end W; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; var tot := 0; "
        "    var s := \"abc\" + 7.ToString(); "
        "    while i < 50 do "
        "      i := i + 1; "
        "      tot := tot + await C.W(s); "
        "    end while; "
        "    if tot = 50 * 4 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8647 (§21.4 / §28.9): a `for` over a +1 string is the string's only
// holder and releases it on every exit: completion, `break`, `continue`,
// and a `return` from inside the body. 200x.
BEZEL_TEST(codegen_str_construct_release, string_producer_for_iterable_is_released_8647_spec_21_4) {
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
        "  public static method First(h: H): int "
        "    for var c in h.Long() do "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end First; "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var s := \"local\" + 1.ToString(); "
        "    var h := new H(7); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      var n := 0; "
        "      for var c in h.Long() do n := n + 1; end; "
        "      for var c in s + \"yz\" do n := n + 1; break; end; "
        "      for var c in h.Mint() do "
        "        if c = 'M' then continue; end; "
        "        n := n + 1; "
        "      end; "
        "      n := n + C.First(h); "
        "      if n = 25 + 1 + 2 + 1 then tot := tot + 1; end; "
        "    end while; "
        "    if tot = 200 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8655 (§29.7 / §28.9): an iterator abandoned while suspended inside a
// `for` over a +1 string releases that string from its `Dispose`; a fully
// consumed one releases it once. 200x.
BEZEL_TEST(codegen_str_construct_release, string_producer_released_when_iterator_abandoned_8655_spec_29_7) {
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
        "  public static method W(h: H): IEnumerable<int> "
        "    for var c in h.Long() do "
        "      yield return 1; "
        "    end; "
        "    yield return 100; "
        "  end W; "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var h := new H(7); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      for var v in C.W(h) do tot := tot + v; break; end; "
        "      for var v in C.W(h) do tot := tot + v; end; "
        "    end while; "
        "    if tot = 200 * (1 + 125) then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8656 (§29.3 / §28.4 / §28.9): `yield break` has the effect of falling off
// the end, so every open scope is disposed: the `using var` destructors run
// (the live count returns to zero) and the string locals and the loop-owned
// iterable are released. 200x.
BEZEL_TEST(codegen_str_construct_release, yield_break_runs_scope_disposals_8656_spec_29_3) {
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
        "class R "
        "  public static Live: int; "
        "  public constructor() R.Live := R.Live + 1; end constructor; "
        "  destructor R.Live := R.Live - 1; end destructor; "
        "end R; "
        "class C "
        "  public static method W(h: H, i: int): IEnumerable<int> "
        "    using var r := new R(); "
        "    var k := 0; "
        "    while k < 3 do "
        "      using var q := new R(); "
        "      var t := h.Mint(); "
        "      yield return t.Length; "
        "      if k = 1 then yield break; end if; "
        "      k := k + 1; "
        "    end while; "
        "    yield return 100; "
        "  end W; "
        "  public static method E(h: H): IEnumerable<int> "
        "    for var c in h.Long() do "
        "      yield return 1; "
        "      yield break; "
        "    end; "
        "    yield return 100; "
        "  end E; "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var h := new H(7); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      for var v in C.W(h, i) do tot := tot + v; end; "
        "      for var v in C.E(h) do tot := tot + v; end; "
        "    end while; "
        "    if tot = 200 * 7 and R.Live = 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}
