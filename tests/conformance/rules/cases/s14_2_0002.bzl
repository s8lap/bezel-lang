//! case: s14_2_0002
//! doc: spec
//! section: 14.2
//! title: a record fails where T: class
//! rule: `where T: class` accepts non-nullable reference types only; a value
//!   type is excluded (§14.2).
//! cites: 14.2
//! expect: reject
//! diagnostic: class
//! issue: 4367
module Conf;

public class Box<T> where T: class
  public constructor()
  end constructor;
end Box;

public record Point(X: int, Y: int);

public class Holder
  private _b: Box<Point>?;

  public constructor()
  end constructor;
end Holder;
