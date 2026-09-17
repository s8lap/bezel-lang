//! case: s7_10_0001
//! doc: spec
//! section: 7.10
//! title: a bare unconstrained type-parameter field must be explicitly initialized
//! rule: A field or local of bare type `T` under an unconstrained parameter
//!   follows the non-nullable-reference rules (must-initialize, no vacuous
//!   `null` default), since `T` may be instantiated non-nullable.
//! cites: 7.9
//! expect: reject
//! diagnostic: explicit initializer
module Conf;

public class Box<T>
  private _value: T;

  public constructor()
  end constructor;
end Box;
