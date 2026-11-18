//! case: s7_5_7106
//! doc: spec
//! section: 7.5
//! title: `?.First` on a LinkedList and `?.Last` on a SortedSet bind to a nullable local and compare with `=`
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: 7
//!   4
//!   seven
//!   -2
//!   -2
module Conf;
public class Program
  public static method F(m: LinkedList<int>?, s: SortedSet<int>?)
    var f := m?.First;
    var g := s?.Last;
    Console.WriteLine(f ?? -2);
    Console.WriteLine(g ?? -2);
    if m?.First = 7 then Console.WriteLine("seven"); end;
  end F;
  public static method Main()
    using var l := new LinkedList<int>();
    _ := l.AddLast(7);
    using var s := new SortedSet<int>();
    s.Add(4);
    F(l, s);
    F(null, null);
  end Main;
end Program;
