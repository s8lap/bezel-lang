//! case: s35_3_0021
//! doc: spec
//! section: 35.3
//! title: an operator method expression body under a declared choice return is a declared-result position
//! rule: "the expression body of a member or accessor checked against a declared type (§22.2 ...)" (§35.3, v0.340): the operator `+` declares `Cmp`, so `More(a.n + b.n)` is `Cmp.More(...)`.
//! cites: 22.2, 25.1
//! expect: run
//! output: more 5
module Conf;

public choice Cmp
  Less,
  More(d: int);
end Cmp;

public record V(n: int)
  public static pure method operator +(a: V, b: V): Cmp => More(a.n + b.n);
end V;

public class Program
  public static method Main()
    Console.WriteLine((case new V(2) + new V(3) of Cmp.Less => "less", Cmp.More(var d) => $"more {d}"));
  end Main;
end Program;
