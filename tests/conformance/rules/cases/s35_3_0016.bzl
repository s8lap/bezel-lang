//! case: s35_3_0016
//! doc: spec
//! section: 35.3
//! title: a case with the same name as its choice is constructed bare in a declared-result position
//! rule: "where the declared type — nullability stripped — is a choice instantiation `C<…>` (or non-generic choice `C`) that declares a case named `N`: the form `N(args)` is a case construction of `C<…>.N(args)`, and the bare identifier `N` is the payload-free case value `C<…>.N`" (§35.3, v0.340). The choice `Wrap` declares a case `Wrap(n)`: `return Wrap(n);` is the case construction `Wrap.Wrap(n)` and `return Empty;` the payload-free case.
//! cites: 35.5
//! expect: run
//! output: w 3
//!   e
module Conf;

public choice Wrap
  Wrap(n: int),
  Empty;
end Wrap;

public class Program
  public static method P(n: int): Wrap
    if n = 0 then return Empty; end;
    return Wrap(n);
  end P;

  public static method Main()
    Console.WriteLine((case P(3) of Wrap(var n) => $"w {n}", Empty => "e"));
    Console.WriteLine((case P(0) of Wrap(var n) => $"w {n}", Empty => "e"));
  end Main;
end Program;
