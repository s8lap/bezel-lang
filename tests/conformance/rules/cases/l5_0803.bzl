//! case: l5_0803
//! doc: stdlib
//! section: 5
//! title: Min and Max with a decimal selector over class elements return the smallest and largest decimal
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5
//! expect: run
//! stdout: -4.5 4.5
//! issue: 8108
module Conf;

public class K
  public D: decimal;
  public constructor(d: decimal)
    D := d;
  end constructor;
end K;

public class Program
  public static method Main()
    using var k1 := new K(4.5m);
    using var k2 := new K(-4.5m);
    var k := new Array<K>();
    k.Add(k1);
    k.Add(k2);
    Console.WriteLine($"{k.Min(lambda(x: K) => x.D)} {k.Max(lambda(x: K) => x.D)}");
  end Main;
end Program;
