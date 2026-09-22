//! case: s29_12_0009
//! doc: spec
//! section: 29.12
//! title: a structural call inside a lambda the loop body only defines draws no warning
//! rule: The call must sit in that loop's body — in the loop's own callable
//!   body, not inside a lambda literal or a local type declaration's member
//!   body that the loop body merely contains, whose invocation the loop does
//!   not sequence.
//! cites: 29.12, 27.5, 28.3
//! expect: quiet
module Conf;

public class Program
  public method Demo()
    var xs: Array<int> := [1, 2, 3];
    for var x in xs do
      var grow: () -> () := lambda() => xs.Add(x);
    end;
  end Demo;
end Program;
