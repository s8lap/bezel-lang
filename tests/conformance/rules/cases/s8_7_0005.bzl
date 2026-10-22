//! case: s8_7_0005
//! doc: spec
//! section: 8.7
//! title: the universal members ToString, Equals and GetHashCode remain on Object
//! rule: Only `Dispose()` is "not on `Object`" (§8.7, v0.340); the members every `Object` carries (§8.1: `ToString`, `Equals`, `GetHashCode`) are still declared: for a class instance held as `Object`, `ToString().Length > 0`, `Equals(o)` of itself and a stable hash code all hold.
//! cites: 8.1
//! expect: run
//! output: True
//!   True
//!   True
module Conf;

public class D
  public constructor() end constructor;
end D;

public class Program
  public static method Main()
    var o: Object := new D();
    var s := o.ToString();
    Console.WriteLine(s.Length > 0);
    Console.WriteLine(o.Equals(o));
    var h := o.GetHashCode();
    Console.WriteLine(h = o.GetHashCode());
  end Main;
end Program;
