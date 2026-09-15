//! case: s29_12_0002
//! doc: spec
//! section: 29.12
//! title: an indexer-set to an existing position does not invalidate the enumerator
//! rule: An element-level write that changes no structure does not invalidate:
//!   assigning through the indexer to an existing position (`arr[i] := v`)
//!   leaves the enumerator valid, and a later visit of that position observes
//!   the new value.
//! cites: 29.12
//! expect: run
//! stdout: 12
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var total := 0;
    for var x in xs do
      if x = 1 then
        xs[2] := 9;
      end;
      total := total + x;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
