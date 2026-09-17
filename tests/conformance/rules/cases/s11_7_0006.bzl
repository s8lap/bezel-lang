//! case: s11_7_0006
//! doc: spec
//! section: 11.7
//! title: a closed class may declare abstract members and be subclassed within its own module
//! rule: A class declared with the closed modifier is a closed abstract
//!   class: a class that cannot be instantiated directly and cannot be
//!   inherited from outside the declaring module. A closed class may
//!   declare abstract members without carrying the abstract modifier.
//! cites: 11.7, 12.5
//! expect: accept
module Conf;

public closed class Shape
  public abstract method Area(): double;
end Shape;

public sealed class Circle(radius: double): Shape
  public readonly property Radius: double := radius;

  public override method Area(): double => 3.14159 * Radius * Radius;
end Circle;
