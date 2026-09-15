//! case: s12_1_0004
//! doc: spec
//! section: 12.1
//! title: an override may not narrow a parameter from T? back to T
//! rule: The reverse direction (overridden T?, override T) is rejected, as
//!   in §11.2.
//! cites: 12.1, 11.2
//! expect: reject
//! diagnostic: override
module Conf;

public class Greeter
  public constructor()
  end constructor;

  public virtual method Greet(name: string?): string => $"Hello, {name ?? "stranger"}";
end Greeter;

public class StrictGreeter: Greeter
  public constructor()
  end constructor;

  public override method Greet(name: string): string => $"Hello, {name}";
end StrictGreeter;
