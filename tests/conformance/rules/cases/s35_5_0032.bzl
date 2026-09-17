//! case: s35_5_0032
//! doc: spec
//! section: 35.5
//! title: the using var move adoption in a do-end arm is warning-free
//! rule: The standard remedies are `using var u := move r;` adoption in a `do … end`
//!   arm, an owned-consuming call, an explicit `r.Dispose()`, `_ := r;`.
//! cites: 35.5, 28.3, 28.10
//! expect: quiet
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
        using var u := move w;
        Console.WriteLine("fetched");
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Show;
end Renderer;
