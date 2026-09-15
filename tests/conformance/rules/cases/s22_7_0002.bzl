//! case: s22_7_0002
//! doc: spec
//! section: 22.7
//! title: a derived constructor may chain to a base constructor with base
//! rule: Multiple constructors are permitted; chaining uses `this(...)` or
//!   `base(...)` in the chaining position before the body.
//! cites: 22.7, 12.8
//! expect: accept
module Conf;

public class Shape
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;
end Shape;

public class Circle: Shape
  private _radius: decimal;

  public constructor(radius: decimal)
    base("circle");
    _radius := radius;
  end constructor;
end Circle;
