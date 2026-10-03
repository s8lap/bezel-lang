// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for the imported_literal_target_5328__*.bzl
// rows (issue #5328 / spec §15.4, v0.313). It carries its own trivial RUN
// line per the tests/lit convention (lit collects every `.bzl` under this
// directory, so a companion without one is reported UNRESOLVED — see
// tests/lit/README.md). The RUN line also earns its keep: it asserts the
// EXPORTER compiles, without which every importer row's result would be
// vacuous.
//
// The three statics are the three §15.4 target-list cases the issue
// separates, so one artifact serves all three imported rows:
//
//   SumE — an `IEnumerable<T>` formal. A collection INTERFACE is NOT on
//          §15.4's closed target list ("The target types that admit
//          collection literals are: inline arrays, `Array<T>`/
//          `OwningArray<T>`, `Set<T>`, ... "), so a literal here must be
//          rejected by sema in BOTH legs.
//   St   — a `Set<int>` formal, on the list. `St([1, 2, 2])` must NOT be
//          rejected (its duplicate-collapsing is the codegen arm of the
//          issue, not a sema rejection).
//   Inl  — a `[3]int` formal, on the list. `Inl([1, 2, 3])` must NOT be
//          rejected by sema.
module Lib5328;

public class Ops
  public static method SumE(x: IEnumerable<int>): int
    var s := 0;
    for var v in x do
      s := s + v;
    end for;
    return s;
  end SumE;

  public static method St(s: Set<int>): int => s.Count;

  public static method Inl(a: [3]int): int => a[0] + a[1] + a[2];
end Ops;
