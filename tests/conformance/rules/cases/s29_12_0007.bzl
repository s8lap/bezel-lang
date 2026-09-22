//! case: s29_12_0007
//! doc: spec
//! section: 29.12
//! title: a structural call followed on every path by an exit from the loop draws no warning
//! rule: A structural call followed on every path by an exit from the loop —
//!   `break`, `return`, a `throw` no `catch` within the loop body handles, or
//!   a labeled jump out of it — is exempt, no advance following it.
//! cites: 29.12
//! expect: quiet
module Conf;

public class Program
  public method Demo()
    var xs: Array<int> := [1, 2, 3];
    for var x in xs do
      if x = 2 then
        xs.Add(9);
        break;
      end;
    end;
  end Demo;
end Program;
