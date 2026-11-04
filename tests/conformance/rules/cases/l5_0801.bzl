//! case: l5_0801
//! doc: stdlib
//! section: 5
//! title: Min and Max with a decimal selector over record elements return the smallest and largest decimal
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5
//! expect: run
//! stdout: -2.5 2.5
//! issue: 8108
//! notes: decimal-valued selector over a record element; the projected values are 2.5, -2.5, 0.75.
module Conf;

public record H(N: int, Dec: decimal);

public class Program
  public static method Main()
    var a := new Array<H>();
    a.Add(new H(N := 1, Dec := 2.5m));
    a.Add(new H(N := 2, Dec := -2.5m));
    a.Add(new H(N := -4, Dec := 0.75m));
    Console.WriteLine($"{a.Min(lambda(x: H) => x.Dec)} {a.Max(lambda(x: H) => x.Dec)}");
  end Main;
end Program;
