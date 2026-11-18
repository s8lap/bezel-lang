//! case: l5_6418
//! doc: stdlib
//! section: 5
//! title: Sum through a null-conditional receiver yields a nullable U
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 7.2
//! expect: run
//! stdout: 3.75
module Conf;
public class Program
  public static method Main()
    var a: Array<double>? := new Array<double>();
    a!.Add(1.5); a!.Add(2.25);
    var t := a?.Sum(lambda(x) => x);
    Console.WriteLine(t ?? -1.0);
  end Main;
end Program;
