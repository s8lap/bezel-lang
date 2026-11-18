//! case: s7_5_7302
//! doc: spec
//! section: 7.5
//! title: `?.FirstNode` and `?.LastNode` over a non-empty LinkedList read the
//!   head and tail nodes
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access. The result type is always nullable
//! cites: 7.7, 7.4
//! expect: run
//! output: 5
//!   5
//!   False
//!   False
//!   -3
//!   -4
//!   1
//!   3
//!   False
//!   False
//!   2
//!   2
//! notes: Pins the #8770 fix on the non-null side. A one-element list has one
//!   node, so Next and Previous of it are null. In the three-element list 1,
//!   2, 3 the head is 1, the tail is 3, head.Next is 2 and tail.Previous is
//!   2.
module Conf;

public class Program
  public static method Show(m: LinkedList<int>?)
    Console.WriteLine(m?.FirstNode?.Value ?? -1);
    Console.WriteLine(m?.LastNode?.Value ?? -2);
    var f := m?.FirstNode;
    var l := m?.LastNode;
    Console.WriteLine(f = null);
    Console.WriteLine(l = null);
    Console.WriteLine(m?.FirstNode?.Next?.Value ?? -3);
    Console.WriteLine(m?.LastNode?.Previous?.Value ?? -4);
  end Show;

  public static method Main()
    using var one := new LinkedList<int>();
    using var three := new LinkedList<int>();
    var a := one.AddLast(5);
    var b := three.AddLast(1);
    var c := three.AddLast(2);
    var d := three.AddLast(3);
    Show(one);
    Show(three);
  end Main;
end Program;
