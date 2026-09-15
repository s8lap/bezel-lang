//! case: s31_1_0003
//! doc: spec
//! section: 31.1
//! title: throwing an Object-typed operand is rejected
//! rule: An `Object`-typed operand is likewise rejected (no interface derives
//!   from `Exception`, and an `Object` referent may be a boxed value); narrow
//!   to the exception class first.
//! cites: 31.1
//! expect: reject
//! diagnostic: object
module Conf;

public class BadException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BadException;

public class Program
  public static method MakeObject(): owned Object => new BadException("bad");

  public static method Main()
    var o := MakeObject();
    throw o;
  end Main;
end Program;
