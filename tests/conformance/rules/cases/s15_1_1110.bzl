//! case: s15_1_1110
//! doc: spec
//! section: 15.1
//! title: a generic body using FirstOrNull and MinOrNull on Array<T> is admitted at an int instantiation
//! rule: the `*OrNull` tier (`FirstOrNull`) ... is a conditionally-admitted member where that
//!   type argument is an inline array
//! cites: 14.2
//! expect: run
//! output: 5
//!   5
//! issue: 5911
module Conf;

public class Program
  public static method G<T>(xs: Array<T>): T?
    return xs.FirstOrNull();
  end G;

  public static method H<T>(xs: Array<T>): T? where T: IComparable<T>
    return xs.MinOrNull();
  end H;

  public static method Main()
    using var xs := new Array<int>();
    xs.Add(5);
    Console.WriteLine(Program.G<int>(xs)!);
    Console.WriteLine(Program.H<int>(xs)!);
  end Main;
end Program;
