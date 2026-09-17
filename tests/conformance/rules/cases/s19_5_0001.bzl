//! case: s19_5_0001
//! doc: spec
//! section: 19.5
//! title: a typed binding pattern matches only non-null values
//! rule: A bare type pattern `T` matches only non-null values. A typed
//!   binding pattern `var n: T` also matches only non-null values — `null`
//!   is matched only by the explicit `null` pattern, an untyped `var x`
//!   pattern, or `_`.
//! cites: 19.5, 19.3
//! expect: run
//! stdout: absent
module Conf;

public class Program
  public static method Classify(n: int?): string => (case n of
    var m: int when m > 70 => "big",
    null                   => "absent",
    _                      => "small");

  public static method Main()
    Console.WriteLine(Classify(null));
    Console.WriteLine(Classify(80));
    Console.WriteLine(Classify(5));
  end Main;
end Program;
