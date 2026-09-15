//! case: l5_0006
//! doc: stdlib
//! section: 5
//! title: Take clamps a negative count to zero
//! rule: Take/Skip clamp -- a negative count behaves as 0, and a count
//!   beyond the source takes everything / skips everything; they are range
//!   operations, not magnitude constructors.
//! cites: 5
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var ys := xs.Take(-5).ToArray();
    Console.WriteLine(ys.Count);
  end Main;
end Program;
