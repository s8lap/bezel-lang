//! case: s35_5_0031
//! doc: spec
//! section: 35.5
//! title: an owning-match arm binding left owning auto-drops with the must-use warning
//! rule: In an owning match, an `owned`-payload binding is an **owned local of the
//!   arm body's scope** ...: falling off the arm still owning the value auto-drops
//!   it with the must-use warning naming the binding and the standard remedies.
//! cites: 35.5, 28.3
//! expect: warn
//! diagnostic: must use
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

public class Renderer
  public method Show()
    var outcome := Outcome.Fetched(new Widget());
    case move outcome of
      Fetched(var w) => do
        Console.WriteLine("fetched");
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Show;
end Renderer;
