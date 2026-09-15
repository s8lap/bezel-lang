//! case: s16_6_0001
//! doc: spec
//! section: 16.6
//! title: an inclusive equal-bounds range yields exactly one value, regardless of the step's sign
//! rule: "An inclusive equal-bounds range (`5..5`, `5..5 step 1`, `5..5 step -1`)
//!   yields exactly one value — the bound itself. The step's sign does not
//!   matter; the iteration produces the start, the next computed value (start ±
//!   step) is past the inclusive end, and iteration terminates."
//! cites: 16.6
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    var count: int := 0;
    for var i in 5..5 step -1 do
      count := count + 1;
    end for;
    Console.WriteLine(count);
  end Main;
end Program;
