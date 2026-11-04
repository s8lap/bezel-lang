//! case: l5_0802
//! doc: stdlib
//! section: 5
//! title: Min and Max with a decimal selector after Where aggregate only the filtered elements
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5
//! expect: run
//! stdout: 0.75 2.5
//! issue: 8108
//! notes: Where keeps N=1 (2.5) and N=-4 (0.75).
module Conf;

public record H(N: int, Dec: decimal);

public class Program
  public static method Main()
    var a := new Array<H>();
    a.Add(new H(N := 1, Dec := 2.5m));
    a.Add(new H(N := 2, Dec := -2.5m));
    a.Add(new H(N := -4, Dec := 0.75m));
    var lo := a.Where(lambda(x: H) => x.N <> 2).Min(lambda(x: H) => x.Dec);
    var hi := a.Where(lambda(x: H) => x.N <> 2).Max(lambda(x: H) => x.Dec);
    Console.WriteLine($"{lo} {hi}");
  end Main;
end Program;
