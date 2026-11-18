//! case: s7_5_7101
//! doc: spec
//! section: 7.5
//! title: `?.First` / `?.Last` on a LinkedList evaluates a side-effecting receiver once
//! rule: The receiver is evaluated first; if it is null the access does not occur
//! cites: 7.5, 7.7
//! expect: run
//! output: 11
//!   22
//!   -1
//!   2
module Conf;
public class Counter
  public property N: int := 0;
  public property L: LinkedList<int>? := null;
  public method Get(): LinkedList<int>?
    this.N := this.N + 1;
    return this.L;
  end Get;
end Counter;
public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    _ := l.AddLast(11);
    _ := l.AddLast(22);
    using var cn := new Counter();
    using var cn2 := new Counter();
    cn.L := l;
    Console.WriteLine(cn.Get()?.First ?? -1);
    Console.WriteLine(cn.Get()?.Last ?? -1);
    Console.WriteLine(cn2.Get()?.First ?? -1);
    Console.WriteLine(cn.N);
  end Main;
end Program;
