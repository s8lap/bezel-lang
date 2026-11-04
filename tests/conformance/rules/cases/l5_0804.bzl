//! case: l5_0804
//! doc: stdlib
//! section: 5
//! title: Min and Max with a decimal selector over a one-element source return that element's value for both
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5
//! expect: run
//! stdout: 3.25 3.25
//! issue: 8108
module Conf;

public record H(N: int, Dec: decimal);

public class Program
  public static method Main()
    var one := new Array<H>();
    one.Add(new H(N := 1, Dec := 3.25m));
    Console.WriteLine($"{one.Min(lambda(x: H) => x.Dec)} {one.Max(lambda(x: H) => x.Dec)}");
  end Main;
end Program;
