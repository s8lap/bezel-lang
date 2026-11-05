//! case: l5_1_2102
//! doc: stdlib
//! section: 5.1
//! title: two T-typed Sum() results compare with > in a generic body
//! rule: public method Sum(): owned T;
//! cites: 5, 2.19
//! expect: run
//! output: True
//! notes: Green on base and on the staging tip: the accepting twin of the
//!   #8266 reject rows (Sum() over a type-parameter element is T).
module Conf;

public class Program
  public static method G<T>(a: Array<T>, b: Array<T>): bool where T: INumber<T> => a.Sum() > b.Sum();

  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var b := new Array<int>();
    b.Add(1);
    Console.WriteLine(G(a, b));
  end Main;
end Program;
