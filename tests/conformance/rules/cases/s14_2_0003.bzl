//! case: s14_2_0003
//! doc: spec
//! section: 14.2
//! title: a record satisfies where T: value
//! rule: `where T: value` accepts non-nullable value types — records, enums,
//!   choices, primitives, inline arrays, the unit type (§14.2).
//! cites: 14.2
//! expect: accept
module Conf;

public class Box<T> where T: value
  public constructor()
  end constructor;
end Box;

public record Point(X: int, Y: int);

public class Holder
  private _b: Box<Point>?;

  public constructor()
  end constructor;
end Holder;
