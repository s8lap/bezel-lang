//! case: s14_2_0077
//! doc: spec
//! section: 14.2
//! title: a nested constraint on a class type parameter is rejected for an unconstrained parameter
//! rule: Where a constraint's satisfaction turns on a type parameter in scope, it is decided at the instantiation site that names the argument, by the constraints of the parameters it turns on.
//!   For class C<T> where T: new(), C<W> is admitted in class Box<W> where W: new() and rejected in an unconstrained class Box<W>.
//! cites: 14.2
//! expect: reject
//! diagnostic: constraint
//! issue: 5998
module Conf;

public class Box<T> where T: new()
  public constructor() end constructor;
end Box;

public class Keep<W>
  private _b: Box<W>?;
  public constructor() end constructor;
end Keep;
