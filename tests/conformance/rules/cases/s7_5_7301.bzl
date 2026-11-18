//! case: s7_5_7301
//! doc: spec
//! section: 7.5
//! title: `?.FirstNode` and `?.LastNode` over a null or an empty LinkedList
//!   yield null down the whole chain
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access. The result type is always nullable
//! cites: 7.7, 7.4
//! expect: run
//! output: -1
//!   -2
//!   True
//!   True
//!   -3
//!   -4
//!   -1
//!   -2
//!   True
//!   True
//!   -3
//!   -4
//! notes: Pins the #8770 fix. The receiver is a LinkedList<int>? (stdlib 4.6:
//!   FirstNode and LastNode are LinkedListNode<T>?). A null receiver and a
//!   non-null receiver of an empty list both give null at the first segment,
//!   so each chain is null and each `??` supplies its right operand.
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
    using var e := new LinkedList<int>();
    Show(null);
    Show(e);
  end Main;
end Program;
