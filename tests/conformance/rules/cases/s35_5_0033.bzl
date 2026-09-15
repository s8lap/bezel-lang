//! case: s35_5_0033
//! doc: spec
//! section: 35.5
//! title: an owned-consuming call is an expression-position arm's warning-free discharge
//! rule: In *expression-position* arms (which cannot contain statements, §19.2), the
//!   warning-free discharges are an owned-consuming call (`RenderOwned(move r)`) or
//!   §19.2's helper-method extraction.
//! cites: 35.5, 19.2, 28.3
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
  public method RenderOwned(owned w: Widget)
    using var kept := move w;
    Console.WriteLine("rendered");
  end RenderOwned;

  public method Show()
    case Outcome.Fetched(new Widget()) of
      Fetched(var w) => RenderOwned(move w),
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Show;
end Renderer;
