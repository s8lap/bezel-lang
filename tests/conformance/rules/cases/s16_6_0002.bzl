//! case: s16_6_0002
//! doc: spec
//! section: 16.6
//! title: a half-open equal-bounds range is empty for any step
//! rule: "A half-open equal-bounds range (`5..<5`, `5..<5 step n`) is empty for any
//!   step. The lower bound equals the upper bound and the upper bound is
//!   excluded, so no value is in range."
//! cites: 16.6
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var count: int := 0;
    for var i in 5..<5 step 3 do
      count := count + 1;
    end for;
    Console.WriteLine(count);
  end Main;
end Program;
