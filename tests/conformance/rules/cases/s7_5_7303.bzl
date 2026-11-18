//! case: s7_5_7303
//! doc: spec
//! section: 7.5
//! title: `?.Value`, `?.Next` and `?.Previous` on a nullable LinkedListNode
//!   local are null-conditional
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access. The result type is always nullable
//! cites: 7.7
//! expect: run
//! output: 10
//!   20
//!   -9
//!   -7
//!   True
//!   10
//! notes: Pins the #8770 fix at a node-typed receiver. `n` is `l.FirstNode`
//!   (a LinkedListNode<int>?), `z` is a null node. The Previous of the head
//!   is null.
module Conf;

public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(10);
    var b := l.AddLast(20);
    var n := l.FirstNode;
    Console.WriteLine(n?.Value ?? -1);
    Console.WriteLine(n?.Next?.Value ?? -1);
    Console.WriteLine(n?.Previous?.Value ?? -9);
    var z: LinkedListNode<int>? := null;
    Console.WriteLine(z?.Value ?? -7);
    Console.WriteLine(z?.Next = null);
    var k: int? := n?.Value;
    Console.WriteLine(k ?? 0);
  end Main;
end Program;
