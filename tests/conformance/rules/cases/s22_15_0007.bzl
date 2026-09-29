//! case: s22_15_0007
//! doc: spec
//! section: 22.15
//! title: an owned class-typed return is rejected on a pure member
//! rule: The return type is unrestricted except: an `owned` class-typed
//!   return is rejected.
//! cites: 22.15
//! expect: reject
//! diagnostic: owned
module Conf;

public class Box
  public constructor()
  end constructor;
end Box;

public abstract class Ops
  public pure abstract method Make(): owned Box;
end Ops;
