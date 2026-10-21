//! case: s10_5_0012
//! doc: spec
//! section: 10.5
//! title: an enum's own-typed Equals answers direct calls, hash containers and boxed dispatch
//! rule: "Each **replaces the compiler-generated default for every consumer**: direct calls, hash containers (`Set<E>` and `Dictionary<E, V>` reach the user's `Equals`/`GetHashCode`), ..., and virtual dispatch through a boxed reference ([§8.6])" ([§10.5]).
//! cites: 10.5, 8.6
//! expect: run
//! output: True True
//!   set=1
//!   dict=1 True
//!   True
module Conf;

enum E
  A, B;
  public method Equals(other: E): bool => true;
  public method GetHashCode(): int => 7;
end E;

public class Program
  public static method Main()
    var o: Object := E.A;
    var o2: Object := E.A;
    var o3: Object := E.B;
    Console.WriteLine(o.Equals(o2).ToString() + " " + o.Equals(o3).ToString());
    var s := new Set<E>();
    s.Add(E.A);
    s.Add(E.A);
    s.Add(E.B);
    Console.WriteLine("set=" + s.Count.ToString());
    var d := new Dictionary<E, int>();
    d[E.A] := 1;
    d[E.B] := 2;
    d[E.A] := 3;
    Console.WriteLine("dict=" + d.Count.ToString() + " " + d.ContainsKey(E.B).ToString());
    Console.WriteLine(E.A.Equals(E.B).ToString());
  end Main;
end Program;
