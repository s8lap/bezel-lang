//! case: s30_6_0002
//! doc: spec
//! section: 30.6
//! title: an ascribed async lambda may target OwningTask<T> for a fresh class result
//! rule: A **return-type ascription** names the wrapper: `async
//!   lambda(path): OwningTask<Widget> => ...` — Pattern B return; the
//!   lambda's state machine forwards the owned result through.
//! cites: 30.6, 30.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Setup()
    var loader: (string) -> OwningTask<Widget> := async lambda(path): OwningTask<Widget> => do
      await Task.Delay(0);
      return new Widget();
    end;
  end Setup;
end Program;
