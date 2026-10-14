//! case: l4_4_0045
//! doc: stdlib
//! section: 4.4
//! title: Array<T>.Remove result is bool and does not convert to int
//! rule: public method Remove(item: T): bool; (stdlib §4.4 Array<T>)
//! cites: 4.4
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    var n: int := a.Remove(1);
  end Main;
end Program;
