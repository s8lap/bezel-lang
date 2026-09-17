//! case: s5_3_0004
//! doc: spec
//! section: 5.3
//! title: a nullable cast of a present value that fits converts by the underlying conversion
//! rule: A present value v converts by T(v) under that conversion's own
//!   semantics; the nullable cast form T?(x) is admitted exactly when the
//!   corresponding non-nullable conversion T(v) is admitted.
//! cites: 5.3, 5.2
//! expect: run
//! stdout: [5]
module Conf;

public class Program
  public static method Main()
    var x: int? := 5;
    var b := byte?(x);
    Console.WriteLine($"[{b}]");
  end Main;
end Program;
