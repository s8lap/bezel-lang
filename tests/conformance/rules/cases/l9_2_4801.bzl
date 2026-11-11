//! case: l9_2_4801
//! doc: stdlib
//! section: 9.2
//! title: Array<float>.Sort() is admitted (float is IComparable<float>) and orders by value
//! rule: public record Float: IComparable<float>, IEquatable<float>
//! cites: 4.4, 14.2
//! expect: run
//! output: True
//!   True
//!   True
//!   True
module Conf;

public class Program
  public static method Main()
    var a: Array<float> := [1.5f, 6.0f, 0.25f, -3.5f];
    a.Sort();
    Console.WriteLine(a[0] = -3.5f);
    Console.WriteLine(a[1] = 0.25f);
    Console.WriteLine(a[2] = 1.5f);
    Console.WriteLine(a[3] = 6.0f);
  end Main;
end Program;
