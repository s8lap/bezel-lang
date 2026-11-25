//! case: s28_11_9917
//! doc: spec
//! section: 28.11
//! title: A literal listing an alias local and its source keeps both elements callable
//! rule: runtime-managed reference (`string`, function types, `Type`;
//! cites: 28.11, 28.9, 27.5
//! expect: run
//! stdout: 39800
//! notes: Harvested from the #9333 review (r43-rev-m144-9333). An Array literal of function-typed locals
//!   stores the references (28.11 slot table, shared-reference slots), so each element and the source
//!   local stay callable and each reference is released once. The release of the literal's element
//!   locals is not observable from stdout; this case guards the opposite failure, an over-release on
//!   a shape with more than one holder. The Stack, Queue and Dictionary literal forms, call-argument
//!   position, nested literals and the #9360 #9361 #9362 residuals are not covered here. The alias h
//!   and f are one closure listed twice: 2 x 19900 = 39800.
module Conf;
import System.Collections;

public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0 ..< 200 do
      var k := i;
      var f: () -> int := lambda() => k;
      var h: () -> int := f;
      using var st: Array<() -> int> := [h, f];
      acc := acc + st[0]() + st[1]();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
