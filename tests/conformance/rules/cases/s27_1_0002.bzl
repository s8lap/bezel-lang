//! case: s27_1_0002
//! doc: spec
//! section: 27.1
//! title: a method with no return type has function type returning the unit type
//! rule: A method without a return type has function type returning `()`;
//!   `method Foo()` parallels `() -> ()`.
//! cites: 27.1
//! expect: accept
module Conf;

public class Logger
  private _sink: () -> ();

  public constructor()
    _sink := lambda() => Console.WriteLine("tick");
  end constructor;

  public method Fire()
    _sink();
  end Fire;
end Logger;
