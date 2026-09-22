//! case: s29_12_0005
//! doc: spec
//! section: 29.12
//! title: a structural call on the iterated binding draws the mutation-during-iteration warning
//! rule: The compiler must warn at a call to a structural operation whose
//!   receiver is the same binding the enclosing `for` iterates, when the call
//!   sits in that loop's body and the loop's next advance is reachable from
//!   it; the call is warned whether or not it is conditional.
//! cites: 29.12, 29.2
//! expect: warn
//! diagnostic: structural
module Conf;

public class Program
  public method Demo()
    var xs: Array<int> := [1, 2, 3];
    for var x in xs do
      if x = 2 then
        xs.Add(9);
      end;
    end;
  end Demo;
end Program;
