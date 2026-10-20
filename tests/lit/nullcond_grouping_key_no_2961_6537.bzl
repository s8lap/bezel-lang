// fix #6537 (codegen, `?.`-`Key` lowering of an `IGrouping<TKey, T>` receiver,
// spec §7.5 v0.338 / stdlib §5 v0.338): the DIAGNOSTIC-ABSENCE oracle for
// #6537, on the one channel where it is not vacuous.
//
// WHY A LIT FILE AND NOT A GTEST ROW. `runAot` reports Warnings through the
// forked compile's stderr and leaves `JitResult::diagnostics` EMPTY whenever
// `r.ran` is true — so an `EXPECT_FALSE(hasWarning(r))` after a successful
// run is a tautology on ANY build, the #3138 trap
// (`scripts/check_vacuous_tests.py`). The value rows live in
// tests/codegen/nullable_grouping_key_widened_tkey_6537_test.cpp; this file
// carries the other half: `bezelc -c` writes EVERY diagnostic of any severity
// to stderr on an ACCEPT, so `FileCheck` on that stream is a real oracle.
//
// THE DEFECT. The `?.`-`Key` arm gated on the LLVM SHAPE of `TKey` — pointer or
// an integer of at most 32 bits — and returned `nullptr` otherwise. Upstream a
// nullptr RHS is #2961's "store dropped — target retains its previous value",
// a WARNING: the program compiled, ran, and exited 0 printing the wrong answer.
// `float`, `double`, `long`, `ulong` and `decimal` keys all took that path, so
// `g?.Key ?? fallback` used the fallback for a grouping whose key was never
// null. §7.5 restricts the RECEIVER's nullability and names no restriction on
// the member's result type, and stdlib §5 declares `readonly property Key:
// TKey` for every `TKey` — so the access is admitted by the spec and the
// result is `TKey?` (a §7.2 nullable VALUE type, the tagged
// `{i8 hasValue; T}` struct) for all of them.
//
// The `.` form reached the same `bz_grouping_key` intrinsic through the
// MemberAccessExpr arm with no shape gate, which is why `dot=4` printed beside
// `nc=0` on the issue's own repro. This file keeps both spellings side by side
// so the control is visible in the same run, and asserts the compile is SILENT
// — no `internal:`, no `#2961`, no `#3025`, and no diagnostic of any severity —
// for every widened TKey and for every control shape that already worked.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=CLEAN --allow-empty %s

module M;
class C
  public static method Main(): int
    // ── the widened TKey axis: `double`, `float`, `long`, `ulong`, `decimal`
    var xs: Array<double> := [1.5, 2.5];
    var d := 0.0;
    for var g in xs.GroupBy(lambda(x) => x) do
      d := d + (g?.Key ?? 0.0);
    end for;
    Console.WriteLine($"double={d}");

    var fs: Array<float> := [1.5f, 2.5f];
    var f := 0.0f;
    for var g in fs.GroupBy(lambda(x) => x) do
      f := f + (g?.Key ?? 0.0f);
    end for;
    Console.WriteLine($"float={f}");

    var ls: Array<long> := [1L, 2L, 3L];
    var l := 0L;
    for var g in ls.GroupBy(lambda(x) => x) do
      l := l + (g?.Key ?? 0L);
    end for;
    Console.WriteLine($"long={l}");

    var us: Array<ulong> := [1UL, 2UL, 3UL];
    var u := 0UL;
    for var g in us.GroupBy(lambda(x) => x) do
      u := u + (g?.Key ?? 0UL);
    end for;
    Console.WriteLine($"ulong={u}");

    var ms: Array<decimal> := [1.5m, 2.5m];
    var m := 0.0m;
    for var g in ms.GroupBy(lambda(x) => x) do
      m := m + (g?.Key ?? 0.0m);
    end for;
    Console.WriteLine($"decimal={m}");

    // ── the access is PERFORMED, not short-circuited, at a widened TKey: the
    //    receiver is a non-nullable IGrouping, so `k <> null` must hold and the
    //    VALUE must fold in (§7.5 short-circuits on a null RECEIVER only).
    var seen := 0.0;
    for var g in xs.GroupBy(lambda(x) => x) do
      var k: double? := g?.Key;
      if k <> null then seen := seen + (k ?? -1.0); end;
    end for;
    Console.WriteLine($"seen={seen}");

    // ── CONTROLS: the shapes that already worked and must stay silent and
    //    correct. `int`/`byte`/`short` are the ≤32-bit-integer gate's interior;
    //    `char` is an i32-lowering value type (§7.1, #849); `string` must stay
    //    a BARE nullable reference — `(g?.Key ?? "?").Length` is the shape
    //    #6512's review measured as breaking when the key was wrapped in the
    //    tagged struct instead ("GEP base pointer is not a vector or a vector
    //    of pointers").
    var ints: Array<int> := [1, 2, 3];
    var i := 0;
    for var g in ints.GroupBy(lambda(x) => x) do
      i := i + (g?.Key ?? 0);
    end for;
    Console.WriteLine($"int={i}");

    var bs: Array<byte> := [1, 2, 3];
    var b := 0;
    for var g in bs.GroupBy(lambda(x) => x) do
      b := b + int(g?.Key ?? 0);
    end for;
    Console.WriteLine($"byte={b}");

    var cs: Array<char> := ['a', 'b', 'c'];
    var c := 0;
    for var g in cs.GroupBy(lambda(x) => x) do
      c := c + int(g?.Key ?? 'z');
    end for;
    Console.WriteLine($"char={c}");

    var ss: Array<string> := ["ab", "cde"];
    var t := 0;
    for var g in ss.GroupBy(lambda(x) => x) do
      t := t + (g?.Key ?? "?").Length;
    end for;
    Console.WriteLine($"string={t}");

    // ── the `.`-form CONTROL beside the `?.` form on the very same grouping.
    var dot := 0.0;
    for var g in xs.GroupBy(lambda(x) => x) do
      dot := dot + g.Key;
    end for;
    Console.WriteLine($"dot={dot}");

    return 0;
  end Main;
end C;

// CLEAN-NOT: error
// CLEAN-NOT: warning
// CLEAN-NOT: internal:
// CLEAN-NOT: #2961
// CLEAN-NOT: #3025