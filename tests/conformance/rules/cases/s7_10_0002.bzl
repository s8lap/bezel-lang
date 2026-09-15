//! case: s7_10_0002
//! doc: spec
//! section: 7.10
//! title: a bare unconstrained type-parameter field initialized from a constructor parameter is admitted
//! rule: A field or local of bare type `T` under an unconstrained parameter
//!   follows the non-nullable-reference rules (must-initialize); assignment
//!   in the constructor satisfies it.
//! cites: 7.9
//! expect: accept
module Conf;

public class Box<T>
  private _value: T;

  public constructor(value: T)
    _value := value;
  end constructor;
end Box;
