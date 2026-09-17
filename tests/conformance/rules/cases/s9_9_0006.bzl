//! case: s9_9_0006
//! doc: spec
//! section: 9.9
//! title: default initialization of constructor-only-record storage is rejected
//! rule: A constructor-only record is not defaultable, so default initialization
//!   of `R`-typed storage is rejected: the field has no zero value to hold and no
//!   constructor behind one.
//! cites: 7.9, 17.11
//! expect: reject
//! diagnostic: constructor-only
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0)

    Value := value;
  end constructor;
end Port;

public class Listener
  private _port: Port;

  public constructor()
  end constructor;
end Listener;
