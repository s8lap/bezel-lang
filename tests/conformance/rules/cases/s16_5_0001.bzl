//! case: s16_5_0001
//! doc: spec
//! section: 16.5
//! title: an open-ended range literal directly in a for statement warns it cannot be iterated
//! rule: "The compiler emits a warning when an open-ended range literal appears in
//!   an iteration context" — `for var i in 10.. do ... end;  // WARNING:
//!   open-ended range cannot be iterated`.
//! cites: 16.5
//! expect: warn
//! diagnostic: cannot be iterated
module Conf;

public class Program
  public static method Main()
    for var i in 10.. do
      Console.WriteLine(i);
    end for;
  end Main;
end Program;
