//! case: s28_6_0023
//! doc: spec
//! section: 28.6
//! title: reading default-iteration elements without disposing them is admitted
//! rule: A default-iteration variable over a borrowing-family collection is a
//!   borrow of the slot; ordinary borrow use (without disposal) is admitted.
//! cites: 28.6, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Cleanup(pool: Array<Widget>)
    for var w in pool do
      Console.WriteLine("visit");
    end;
  end Cleanup;
end Program;
