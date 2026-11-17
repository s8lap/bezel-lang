//! case: l1_2302
//! doc: stdlib
//! section: 1
//! title: Where(pred).Count() on an IList reaches the operator pipeline
//! rule: the composed spelling — `coll.Where(pred).Count()`; `Where` is
//!   shadowed nowhere in this document — reaches the operator pipeline
//! cites: 1, 4.1
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3, 4];
    var il: IList<int> := a;
    Console.WriteLine(il.Where(lambda(x: int): bool => x > 1).Count());
  end Main;
end Program;
