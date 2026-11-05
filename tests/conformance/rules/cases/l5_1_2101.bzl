//! case: l5_1_2101
//! doc: stdlib
//! section: 5.1
//! title: a generic body returns the no-selector Sum() over T as T
//! rule: public method Sum(): owned T;
//! cites: 5, 2.19
//! expect: run
//! output: 11
//! notes: Green on base and on the staging tip: the accepting twin of the
//!   #8266 reject rows (Sum() over a type-parameter element is T).
module Conf;

public class Program
  public static method Id<T>(x: T): T => x;
  public static method G<T>(a: Array<T>): T where T: INumber<T> => Id(a.Sum());

  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    Console.WriteLine(G(a));
  end Main;
end Program;
