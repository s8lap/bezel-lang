//! case: l2_2_0045
//! doc: stdlib
//! section: 2.2
//! title: Math.Max over a Math.Abs of a float infers T as double
//! rule: public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
//! cites: 5.1, 14.2
//! expect: run
//! stdout: 0.5
//! notes: Abs(float) has type double (l2_2_0040), and the second argument 0.5 is a double, so both arguments of Max have type double and T is double. Abs(-0.25f) is 0.25 (exact), and the larger of 0.25 and 0.5 is 0.5.
module Conf;

public class Program
  public static method Main()
    var f: float := -0.25f;
    var m := Math.Max(Math.Abs(f), 0.5);
    Console.WriteLine(m);
  end Main;
end Program;
