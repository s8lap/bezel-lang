//! case: s35_5_0001
//! doc: spec
//! section: 35.5
//! title: the borrowing match is the default and leaves the scrutinee unchanged
//! rule: Matching a choice value non-destructively is the default everywhere: a
//!   pattern binding is a borrow whose source is the scrutinee's source; the
//!   scrutinee retains its ownership state; the construct never moves from the
//!   scrutinee.
//! cites: 35.5, 19.3
//! expect: run
//! output: a
//!   dispose a
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  public method Name(): string => _name;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed(status: int);
end Outcome;

public class Program
  public static method Main()
    var outcome := Outcome.Fetched(new Widget("a"));
    case outcome of                       // unmarked named binding: a borrowing match
      Fetched(var w) => Console.WriteLine(w.Name()),
      Failed(_)      => Console.WriteLine("failed")
    end case;
    _ := outcome;                         // still owned here — the discard disposes it
  end Main;
end Program;
