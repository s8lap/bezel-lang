//! case: s15_1_1105
//! doc: spec
//! section: 15.1
//! title: the *OrNull tier over int elements, including lazy Where and Min/Max, is admitted
//! rule: the `*OrNull` tier (`FirstOrNull`) ... is a conditionally-admitted member where that
//!   type argument is an inline array
//! cites: 14.2
//! expect: run
//! output: 5
//!   2
//!   2
//!   5
//!   True
//!   5
//! issue: 5911
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    xs.Add(5);
    xs.Add(2);
    Console.WriteLine(xs.FirstOrNull()!);
    Console.WriteLine(xs.LastOrNull()!);
    Console.WriteLine(xs.MinOrNull()!);
    Console.WriteLine(xs.MaxOrNull()!);
    using var e := new Array<int>();
    Console.WriteLine(e.FirstOrNull() = null);
    Console.WriteLine(xs.Where(lambda(x: int) => x > 3).FirstOrNull()!);
  end Main;
end Program;
