//! case: s7_5_9003
//! doc: spec
//! section: 7.5
//! title: the `?.` spelling of LinkedList's FirstNode, chained into the node's Value, is admitted
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 26.5
//! expect: run
//! output: 10
//! notes: Harvested from the #8783 producer's corpus-candidates (l4_6_9001; its stated output "1" was wrong,
//!   the first node holds 10). Stdlib 4.6 declares `FirstNode` on `LinkedList<T>` and `Value` on
//!   `LinkedListNode<T>`. Trace: the list is [10, 20]; `q?.FirstNode?.Value` is 10.
module Conf;
public class Program
  public static method Main()
    var l: LinkedList<int> := [10, 20];
    var q: LinkedList<int>? := l;
    Console.WriteLine(q?.FirstNode?.Value ?? -1);
  end Main;
end Program;
