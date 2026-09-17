//! case: l2_7_0001
//! doc: stdlib
//! section: 2.7
//! title: two Random instances constructed with the same seed agree
//! rule: Random is seeded; constructing it with an explicit seed determines
//!   its output sequence, as distinct from the default constructor's
//!   system-time-based seed.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var r1 := new Random(42);
    var r2 := new Random(42);
    Console.WriteLine(r1.Next() = r2.Next());
  end Main;
end Program;
