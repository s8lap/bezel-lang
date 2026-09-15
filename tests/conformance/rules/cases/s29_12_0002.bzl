//! case: s29_12_0002
//! doc: spec
//! section: 29.12
//! title: an indexer-set to an existing position does not invalidate the enumerator
//! rule: An element-level write that changes no structure does not
//!   invalidate: assigning through the indexer to an existing position
//!   (`arr[i] := v`) leaves the enumerator valid, and a later visit of that
//!   position observes the new value.
//! cites: 29.12
//! expect: run
//! stdout: 13
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    for var x in xs do
      if x = 2 then
        xs[1] := 9;
      end;
    end;
    Console.WriteLine(xs[0] + xs[1] + xs[2]);
  end Main;
end Program;
