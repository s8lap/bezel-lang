//! case: s15_1_1108
//! doc: spec
//! section: 15.1
//! title: the *OrNull tier over enum elements and enum dictionary values is admitted
//! rule: the `*OrNull` tier (`FirstOrNull`) ... is a conditionally-admitted member where that
//!   type argument is an inline array
//! cites: 14.2
//! expect: run
//! output: True
//!   True
//! issue: 5911
module Conf;

public enum E
  A, B;
end E;

public class Program
  public static method Main()
    using var xs := new Array<E>();
    xs.Add(E.B);
    Console.WriteLine(xs.FirstOrNull()! = E.B);
    using var d := new Dictionary<string, E>();
    d.Add("k", E.A);
    Console.WriteLine(d.GetOrNull("k")! = E.A);
  end Main;
end Program;
