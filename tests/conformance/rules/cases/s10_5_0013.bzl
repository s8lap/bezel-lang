//! case: s10_5_0013
//! doc: spec
//! section: 10.5
//! title: a plain Equals at another parameter type replaces nothing; Object.Equals keeps the default
//! rule: "A plain `Equals` at any other parameter type is an ordinary instance method, statically dispatched, that replaces nothing and discharges neither §8.3's pairing nor §25.2's co-declaration obligation" ([§10.5]); an enum's default `Equals` is underlying integer equality ([§8.3]).
//! cites: 10.5, 8.3
//! expect: run
//! stdout: same=100 diff=0
//! issue: 6202
module Conf;

enum E
  A, B;
  public method Equals(other: int): bool => other = 42;
end E;

public class Program
  public static method Main()
    var n := 0;
    var m := 0;
    for var j in 0..<100 do
      var o: Object := E.A;
      var o2: Object := E.A;
      var o3: Object := E.B;
      if o.Equals(o2) then n := n + 1; end;
      if o.Equals(o3) then m := m + 1; end;
    end for;
    Console.WriteLine("same=" + n.ToString() + " diff=" + m.ToString());
  end Main;
end Program;
