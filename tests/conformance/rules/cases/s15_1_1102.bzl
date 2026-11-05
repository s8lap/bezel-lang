//! case: s15_1_1102
//! doc: spec
//! section: 15.1
//! title: SingleOrNull on a Take over inline-array elements is a compile error
//! rule: a standard-library member whose **signature** forms `T?` at a type parameter of its
//!   receiver type ... is a conditionally-admitted member where that type argument is an
//!   inline array ... a compile error at the use
//! cites: 14.2
//! expect: reject
//! diagnostic: inline array
//! issue: 5911
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<[3]int>();
    var f := xs.Take(2).SingleOrNull();
  end Main;
end Program;
