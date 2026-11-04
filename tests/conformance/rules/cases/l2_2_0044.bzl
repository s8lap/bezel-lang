//! case: l2_2_0044
//! doc: stdlib
//! section: 2.2
//! title: Math.Max of two floats is generic and keeps its float type
//! rule: public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
//! cites: 14.2, 22.6
//! expect: run
//! stdout: float
//! notes: Control for l2_2_0043: Max is not one of the per-type overloads, so with two float arguments T is float, the call has type float, and W(float) is the exact match.
module Conf;

public class Program
  public static method W(x: float): string => "float";
  public static method W(x: double): string => "double";

  public static method Main()
    var f: float := -2.5f;
    Console.WriteLine(W(Math.Max(f, 0.0f)));
  end Main;
end Program;
