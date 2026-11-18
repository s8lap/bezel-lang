//! case: s5_1_6419
//! doc: spec
//! section: 5.1
//! title: a generic-body Sum result of type T is not implicitly convertible to int
//! rule: a type parameter to an interface or class its constraints guarantee
//!   conformance to
//! cites: 5, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;
public class Program
  static method H<T>(a: Array<T>): int where T: INumber<T>
    return a.Sum(lambda(x) => x);
  end H;
  public static method Main()
    var a := new Array<int>();
    a.Add(3);
    Console.WriteLine(H(a));
  end Main;
end Program;
