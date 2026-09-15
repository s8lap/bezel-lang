//! case: l4_6_0004
//! doc: stdlib
//! section: 4.6
//! title: LinkedList<T>.Add appends at the tail, equivalent to AddLast
//! rule: Plus the standard ICollection<T> operations: Add (appends at the
//!   tail, = AddLast — v0.195).
//! cites: 4.6
//! expect: run
//! stdout: 30
module Conf;

public class Program
  public static method Main()
    using var l: LinkedList<int> := [10, 20];
    l.Add(30);
    Console.WriteLine(l.Last);
  end Main;
end Program;
