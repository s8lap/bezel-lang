//! case: s15_1_1201
//! doc: spec
//! section: 15.1
//! title:ElementAtOrNull on a lazy Where over inline-array elements is rejected
//! rule:a standard-library member whose **signature** forms `T?` at a type parameter of its
//!   receiver type
//! cites: 14.2
//! expect: reject
//! diagnostic: inline array
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<[3]int>();
    var a: [3]int := [1, 2, 3];
    xs.Add(a);
    var v := xs.Where(lambda(x) => true).ElementAtOrNull(0);
    Console.WriteLine(v <> null);
  end Main;
end Program;
