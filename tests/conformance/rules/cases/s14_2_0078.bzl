//! case: s14_2_0078
//! doc: spec
//! section: 14.2
//! title: the same nested constraint is admitted when the class type parameter carries it
//! rule: Where a constraint's satisfaction turns on a type parameter in scope, it is decided by the constraints of that parameter; a parameter carrying where W: new() satisfies the nested where T: new().
//! cites: 14.2
//! expect: accept
//! issue: 5998
module Conf;

public class Box<T> where T: new()
  public constructor() end constructor;
end Box;

public class Keep<W> where W: new()
  private _b: Box<W>?;
  public constructor() end constructor;
end Keep;
