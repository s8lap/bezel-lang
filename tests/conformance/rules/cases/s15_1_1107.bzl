//! case: s15_1_1107
//! doc: spec
//! section: 15.1
//! title: the *OrNull tier over string elements and string dictionary values is admitted
//! rule: the `*OrNull` tier (`FirstOrNull`) ... is a conditionally-admitted member where that
//!   type argument is an inline array
//! cites: 14.2
//! expect: run
//! output: a
//!   a
//!   v
//!   True
//! issue: 5911
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<string>();
    xs.Add("a");
    Console.WriteLine(xs.FirstOrNull()!);
    Console.WriteLine(xs.MinOrNull()!);
    using var d := new Dictionary<string, string>();
    d.Add("k", "v");
    Console.WriteLine(d.GetOrNull("k")!);
    Console.WriteLine(d.GetOrNull("z") = null);
  end Main;
end Program;
