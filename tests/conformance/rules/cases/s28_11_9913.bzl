//! case: s28_11_9913
//! doc: spec
//! section: 28.11
//! title: A literal listing one function local three times keeps every element callable
//! rule: runtime-managed reference (`string`, function types, `Type`;
//! cites: 28.11, 28.9, 27.5
//! expect: run
//! stdout: 79600
//! notes: Harvested from the #9333 review (r43-rev-m144-9333). An Array literal of function-typed locals
//!   stores the references (28.11 slot table, shared-reference slots), so each element and the source
//!   local stay callable and each reference is released once. The release of the literal's element
//!   locals is not observable from stdout; this case guards the opposite failure, an over-release on
//!   a shape with more than one holder. The Stack, Queue and Dictionary literal forms, call-argument
//!   position, nested literals and the #9360 #9361 #9362 residuals are not covered here. Each
//!   iteration reads k four times: 4 x 19900 = 79600.
module Conf;
import System.Collections;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      var k := i;
      var f: () -> int := lambda() => k;
      using var st: Array<() -> int> := [f, f, f];
      acc := acc + st[0]() + st[1]() + st[2]() + f();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
