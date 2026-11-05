//! case: l5_2001
//! doc: stdlib
//! section: 5
//! title: re-enumerating a bound lazy query by two straight-line Count
//!   calls is a compile error
//! rule: it may be enumerated at most once (a re-enumeration is a compile
//!   error, or `InvalidOperationException` where only dynamically
//!   reachable)
//! cites: 29.11
//! expect: reject
//! diagnostic: consumed
module Conf;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    var q := a.Where(lambda(x) => x > 0);
    var t := 0;
    t := t + q.Count();
    t := t + q.Count();
    Console.WriteLine(t);
  end Main;
end Program;
