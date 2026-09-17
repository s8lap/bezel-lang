//! case: s29_11_0032
//! doc: spec
//! section: 29.11
//! title: a statically visible consumption point suppresses the query's must-use warning
//! rule: A bound query is spelled `using var q := xs.Where(…);` where the frame is
//!   to be scope-disposed, and K3.14's must-use warning is **suppressed where the
//!   binding's consumption point is statically visible** (the value was used; the
//!   auto-drop still runs).
//! cites: 29.11, 28.3
//! expect: quiet
module Conf;

public class Scanner
  public method Scan(xs: Array<int>)
    var q := xs.Where(lambda(x) => x > 0);
    for var x in q do
      Console.WriteLine($"{x}");
    end for;
  end Scan;
end Scanner;
