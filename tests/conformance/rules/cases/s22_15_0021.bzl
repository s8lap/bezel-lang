//! case: s22_15_0021
//! doc: spec
//! section: 22.15
//! title: a pure body's for loop over a concrete Array<T> source is the normative elision
//! rule: A source whose static type at this body's own check site is
//!   concretely `Array<T>`, `OwningArray<T>`, class-typed
//!   `OwningBuffer<T>`, or `string` — the admitted protocol elision is
//!   likewise the normative semantics inside a pure body.
//! cites: 22.15, 29.2
//! expect: run
//! stdout: 6
module Conf;

public class Ops
  public static pure method Sum(xs: Array<int>): int
    var total := 0;
    for var x in xs do
      total := total + x;
    end;
    return total;
  end Sum;
end Ops;

public class Program
  public static method Main()
    Console.WriteLine(Ops.Sum([1, 2, 3]));
  end Main;
end Program;
