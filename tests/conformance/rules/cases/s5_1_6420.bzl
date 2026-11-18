//! case: s5_1_6420
//! doc: spec
//! section: 5.1
//! title: a long-valued Sum result is not implicitly narrowed to an int local
//! rule: A value of one type implicitly converts to another when no
//!   programmer-visible information is lost
//! cites: 5.1, 14.5
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;
public class Program
  public static method Main()
    var a := new Array<long>();
    a.Add(3000000000L);
    var t: int := a.Where(lambda(x) => true).Sum(lambda(x) => x);
    Console.WriteLine(t);
  end Main;
end Program;
