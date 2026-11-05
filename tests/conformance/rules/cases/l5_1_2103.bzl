//! case: l5_1_2103
//! doc: stdlib
//! section: 5.1
//! title: a class-level T method returns the no-selector Sum() as T
//! rule: public method Sum(): owned T;
//! cites: 5, 2.19
//! expect: run
//! output: 11
//! notes: Green on base and on the staging tip: the accepting twin of the
//!   #8266 reject rows (Sum() over a type-parameter element is T).
module Conf;

public class Acc<T> where T: INumber<T>
  public method Total(a: Array<T>): T => a.Sum();
end Acc;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var k := new Acc<int>();
    Console.WriteLine(k.Total(a));
  end Main;
end Program;
