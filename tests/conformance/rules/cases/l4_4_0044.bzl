//! case: l4_4_0044
//! doc: stdlib
//! section: 4.4
//! title: Array<T>.Remove's result is a bool, usable in a condition
//! rule: public method Remove(item: T): bool; (stdlib §4.4 Array<T>)
//! cites: 4.4
//! expect: run
//! output: ok
module Conf;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(1);
    var b: bool := s.Remove(1);
    if b then Console.WriteLine("ok"); else Console.WriteLine("ok"); end if;
  end Main;
end Program;
