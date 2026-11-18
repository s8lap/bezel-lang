//! case: s7_5_7305
//! doc: spec
//! section: 7.5
//! title: a `?.` chain evaluates its receiver expression exactly once
//! rule: The receiver is evaluated first; if it is null the access does not
//!   occur and nothing to its right in the chain is evaluated
//! cites: 7.7
//! expect: run
//! output: 8
//!   8
//!   2
//!   -1
//!   True
//!   2
//! notes: Pins the #8770 fix: the receiver `h.Get()` is a call with a visible
//!   side effect (it counts its calls), written once per chain. Two chains on
//!   `h` give two calls in total, two chains on the null-holding `z` give two
//!   calls in total.
module Conf;

public class Holder
  public Calls: int;
  public Inner: LinkedList<int>?;

  public constructor(i: LinkedList<int>?)
    Calls := 0;
    Inner := i;
  end constructor;

  public method Get(): LinkedList<int>?
    Calls := Calls + 1;
    return Inner;
  end Get;
end Holder;

public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(8);
    var b := l.AddLast(9);
    using var h := new Holder(l);
    using var z := new Holder(null);
    Console.WriteLine(h.Get()?.FirstNode?.Value ?? -1);
    Console.WriteLine(h.Get()?.LastNode?.Previous?.Value ?? -1);
    Console.WriteLine(h.Calls);
    Console.WriteLine(z.Get()?.FirstNode?.Value ?? -1);
    var nd: LinkedListNode<int>? := z.Get()?.LastNode;
    Console.WriteLine(nd = null);
    Console.WriteLine(z.Calls);
  end Main;
end Program;
