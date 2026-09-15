//! case: s35_5_0003
//! doc: spec
//! section: 35.5
//! title: an owning match is triggered by a named owned binding under `case move`
//! rule: The owning match is triggered by a named owned binding under an explicit
//!   scrutinee transfer marker — case move outcome of ...; the consumed binding is
//!   left moved-from when the construct completes.
//! cites: 35.5, 28.8.1
//! expect: run
//! output: adopted
//!   widget disposed
//!   after
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed(status: int);
end Outcome;

public class Program
  public static method Main()
    var outcome := Outcome.Fetched(new Widget());
    case move outcome of                     // marked consume; `outcome` moved-from after `end`
      Fetched(var w) => do
        using var widget := move w;
        Console.WriteLine("adopted");
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
    Console.WriteLine("after");              // `outcome` is moved-from: nothing drops at scope end
  end Main;
end Program;
