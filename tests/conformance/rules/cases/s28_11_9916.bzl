//! case: s28_11_9916
//! doc: spec
//! section: 28.11
//! title: A literal mixing a function local with a fresh lambda keeps every element callable
//! rule: runtime-managed reference (`string`, function types, `Type`;
//! cites: 28.11, 28.9, 27.5
//! expect: run
//! stdout: 40400
//! notes: Harvested from the #9333 review (r43-rev-m144-9333). An Array literal of function-typed locals
//!   stores the references (28.11 slot table, shared-reference slots), so each element and the source
//!   local stay callable and each reference is released once. The release of the literal's element
//!   locals is not observable from stdout; this case guards the opposite failure, an over-release on
//!   a shape with more than one holder. The Stack, Queue and Dictionary literal forms, call-argument
//!   position, nested literals and the #9360 #9361 #9362 residuals are not covered here. Per
//!   iteration k + 3 + k: 2 x 19900 + 3 x 200 = 40400.
module Conf;
import System.Collections;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      var k := i;
      var f: () -> int := lambda() => k;
      using var st: Array<() -> int> := [f, lambda() => 3, f];
      acc := acc + st[0]() + st[1]() + st[2]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
