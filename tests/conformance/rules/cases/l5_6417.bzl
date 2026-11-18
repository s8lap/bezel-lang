//! case: l5_6417
//! doc: stdlib
//! section: 5
//! title: a Sum nested in a Sum selector infers U = long at both levels
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 14.5
//! expect: run
//! stdout: 12000000000
module Conf;
public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(3); a.Add(4);
    var t: long := a.Sum(lambda(x) => a.Sum(lambda(y) => 3000000000L));
    Console.WriteLine(t);
  end Main;
end Program;
