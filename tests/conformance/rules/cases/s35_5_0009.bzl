//! case: s35_5_0009
//! doc: spec
//! section: 35.5
//! title: a marked named slot in a borrowing match is rejected
//! rule: The marker on a slot of a borrowing match is a compile error: the scrutinee
//!   retains ownership, so no slot can mint an obligation from it.
//! expect: reject
//! diagnostic: borrowing match
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned response: Widget),
  Failed(status: int);
end Outcome;

public class Program
  public static method Peek(outcome: Outcome)
    case outcome of     // unmarked scrutinee: a borrowing match, whatever `outcome`'s own state
      Fetched(move response = var r) => Console.WriteLine("ok"),   // ERROR: no obligation to transfer
      Failed(_) => Console.WriteLine("failed")
    end case;
    _ := outcome;
  end Peek;

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
