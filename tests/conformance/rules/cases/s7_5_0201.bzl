//! case: s7_5_0201
//! doc: spec
//! section: 7.5
//! title: a call suffix right of ?. is typed at the preceding segment's own non-nullable function type
//! rule: a suffix to the right of a `?.` is typed at the value it meets on the path where it runs — the
//!   preceding segment's own declared type, never the nullable type the `?.` gives the chain's result
//! cites: 7.4, 7.6
//! expect: run
//! output: 101
//! issue: 8099
module Conf;

public class D
  public method Mk(): (int) -> int
    return lambda(x: int) => x + 100;
  end Mk;
end D;

public class Program
  public static method Main()
    using var d := new D();
    var p: D? := d;
    var r := p?.Mk()(1);
    Console.WriteLine(r ?? -1);
  end Main;
end Program;
