//! case: s7_3_0005
//! doc: spec
//! section: 7.3
//! title: the null literal at Math.Max's explicitly int-typed parameter is rejected
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: null
//! issue: 7660
//! notes: stdlib 2.2 declares Max<T>(a: T, b: T); with T written int the parameter a is a non-nullable int and null is assigned to it.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Max<int>(null, 5));
  end Main;
end Program;
