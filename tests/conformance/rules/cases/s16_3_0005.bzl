//! case: s16_3_0005
//! doc: spec
//! section: 16.3
//! title: Contains agrees with the direction of iteration, not with unordered-interval membership
//! rule: "an iteration-empty range contains nothing: `50 in 100..0` is `false`
//!   (matching Rust and Python), while `(100..0 step -1).Contains(50)` is `true`
//!   (descending and nonempty)."
//! cites: 16.3, 17.5
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(50 in 100..0);
    Console.WriteLine((100..0 step -1).Contains(50));
  end Main;
end Program;
