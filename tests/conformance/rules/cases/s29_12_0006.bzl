//! case: s29_12_0006
//! doc: spec
//! section: 29.12
//! title: collect-then-remove mutates outside the iterating loop and draws no warning
//! rule: The warning names the remedies: collect the matches and modify
//!   after the loop — the standard pattern for removing during iteration. A
//!   structural call whose receiver is not the binding the enclosing loop
//!   iterates draws nothing.
//! cites: 29.12
//! expect: quiet
module Conf;

public class Program
  public method Demo()
    var xs: Array<int> := [1, 2, 3, 4];
    var evens := new Array<int>();
    for var x in xs do
      if x mod 2 = 0 then
        evens.Add(x);
      end;
    end;
    for var e in evens do
      _ := xs.Remove(e);
    end;
  end Demo;
end Program;
