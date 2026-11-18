//! case: l5_6412
//! doc: stdlib
//! section: 5
//! title: generic-body Sum over ulong elements returns T, directly and after Where
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 14.5
//! expect: run
//! output: 9000000000000000001
//!   9000000000000000001
module Conf;
public class Program
  static method H<T>(a: Array<T>): T where T: INumber<T>
    return a.Sum(lambda(x) => x);
  end H;
  static method L<T>(a: Array<T>): T where T: INumber<T>
    return a.Where(lambda(x) => true).Sum(lambda(x) => x);
  end L;
  public static method Main()
    var a := new Array<ulong>();
    a.Add(9000000000000000000UL); a.Add(1UL);
    Console.WriteLine(H(a));
    Console.WriteLine(L(a));
  end Main;
end Program;
