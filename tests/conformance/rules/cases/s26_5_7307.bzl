//! case: s26_5_7307
//! doc: spec
//! section: 26.5
//! title: the call spelling `q?.FirstNode()` on the property FirstNode has no
//!   candidate
//! rule: If `x`'s declared type (or any base type) *declares* any instance
//!   member named `Foo` — of any arity, applicability, or accessibility —
//!   instance resolution is committed
//! cites: 7.5, 27.7
//! expect: reject
//! diagnostic: FirstNode
//! notes: Control for the #8770 fix: the `?.` call spelling `q?.FirstNode()`
//!   was refused before the fix and must stay refused. FirstNode is a
//!   property (stdlib 4.6), not a method, so the committed instance lookup
//!   finds no applicable member. The accepting twin is the `.FirstNode` read
//!   in l4_6_7306.
module Conf;

public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(5);
    var q: LinkedList<int>? := l;
    Console.WriteLine(q?.FirstNode());
  end Main;
end Program;
