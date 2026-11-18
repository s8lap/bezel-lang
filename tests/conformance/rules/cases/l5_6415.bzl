//! case: l5_6415
//! doc: stdlib
//! section: 5
//! title: generic-body Sum over decimal elements returns T, directly and after Where
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 14.5
//! expect: run
//! output: 3.3
//!   3.3
module Conf;
public class Program
  static method H<T>(a: Array<T>): T where T: INumber<T>
    return a.Sum(lambda(x) => x);
  end H;
  static method L<T>(a: Array<T>): T where T: INumber<T>
    return a.Where(lambda(x) => true).Sum(lambda(x) => x);
  end L;
  public static method Main()
    var a := new Array<decimal>();
    a.Add(1.1m); a.Add(2.2m);
    Console.WriteLine(H(a));
    Console.WriteLine(L(a));
  end Main;
end Program;
