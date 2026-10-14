//! case: l4_10_0036
//! doc: stdlib
//! section: 4.10
//! title: Set<T>.Remove's result is a bool, usable in a condition
//! rule: method Remove(item: T): bool; (stdlib §4.10 Set<T>, ICollection<T>)
//! cites: 4.10
//! expect: run
//! output: ok
module Conf;

public class Program
  public static method Main()
    var s := new Set<int>();
    s.Add(1);
    var b: bool := s.Remove(1);
    if b then Console.WriteLine("ok"); else Console.WriteLine("ok"); end if;
  end Main;
end Program;
