//! case: s29_12_0008
//! doc: spec
//! section: 29.12
//! title: an element-level indexer write inside the iterating loop is not structural and draws no warning
//! rule: An element-level write that changes no structure does not
//!   invalidate: assigning through the indexer to an existing position leaves
//!   the enumerator valid. The mutation-during-iteration warning fires at a
//!   call to a structural operation of the list above only.
//! cites: 29.12
//! expect: quiet
module Conf;

public class Program
  public method Demo()
    var xs: Array<int> := [1, 2, 3];
    for var x in xs do
      if x = 1 then
        xs[2] := 9;
      end;
    end;
  end Demo;
end Program;
