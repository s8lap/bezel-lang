//! case: s28_1_0006
//! doc: spec
//! section: 28.1
//! title: owned on a function-type parameter slot is admitted and consumes at the call
//! rule: The parameter slot of a function type may carry `owned`: `(owned T) -> ()` —
//!   the modifier is part of the function type's identity, and an invocation
//!   through the slot consumes the argument.
//! cites: 27.1
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private _consume: (owned Widget) -> ();

  public constructor()
    _consume := lambda(owned w: Widget) => w.Dispose();
  end constructor;

  public method Run(owned w: Widget)
    _consume(move w);
  end Run;
end Registry;
