//! case: s22_6_0095
//! doc: spec
//! section: 22.6
//! title: a negative literal that does not fit ulong selects the long constructor overload
//! rule: "literal formation is range-checked"
//! cites: 22.6, 5.1
//! expect: run
//! output: 2
module Conf;

public class C
  public v: int;
  public constructor(x0: ulong) v := 1; end constructor;
  public constructor(x0: long) v := 2; end constructor;
end C;

public class Program
  public static method Main()
    var c := new C(-3_000_000_000);
    Console.WriteLine(c.v);
  end Main;
end Program;
