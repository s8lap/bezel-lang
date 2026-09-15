//! case: s28_1_0006
//! doc: spec
//! section: 28.1
//! title: owned function-type parameter slot is a distinct type from the unmarked form
//! rule: The parameter slot of a function type may carry `owned`; `(T) -> ()` and
//!   `(owned T) -> ()` are distinct types.
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
