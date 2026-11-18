//! case: l5_6416
//! doc: stdlib
//! section: 5
//! title: Sum over a string's chars takes U from each selector's numeric result type
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 14.5
//! expect: run
//! output: 6000000000
//!   3.3
//!   6
module Conf;
public class Program
  public static method Main()
    Console.WriteLine("ab".Sum(lambda(c) => 3000000000L));
    Console.WriteLine("abc".Sum(lambda(c) => 1.1m));
    Console.WriteLine("abc".Sum(lambda(c) => 2));
  end Main;
end Program;
