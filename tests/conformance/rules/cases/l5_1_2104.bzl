//! case: l5_1_2104
//! doc: stdlib
//! section: 5.1
//! title: a Set<T> receiver's no-selector Sum() is T in a generic body
//! rule: public method Sum(): owned T;
//! cites: 5, 2.19
//! expect: run
//! output: 12
//! notes: Green on base and on the staging tip: the accepting twin of the
//!   #8266 reject rows (Sum() over a type-parameter element is T).
module Conf;

public class Program
  public static method G<T>(a: Set<T>): T where T: INumber<T> => a.Sum();

  public static method Main()
    var a := new Set<int>();
    a.Add(5);
    a.Add(7);
    Console.WriteLine(G(a));
  end Main;
end Program;
