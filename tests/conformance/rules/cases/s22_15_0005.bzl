//! case: s22_15_0005
//! doc: spec
//! section: 22.15
//! title: a pure member declares no owned parameter
//! rule: A pure member declares no `ref`, `out`, `out owned`, `owned`,
//!   or `scoped` parameters: `owned` brings a disposal obligation into
//!   the frame whose discharge runs `Dispose` — user code at disposal
//!   time.
//! cites: 22.15
//! expect: reject
//! diagnostic: owned
module Conf;

public class Box
  public constructor()
  end constructor;
end Box;

public class Ops
  public static pure method Consume(owned b: Box): int => 0;
end Ops;
