//! case: s31_1_0004
//! doc: spec
//! section: 31.1
//! title: throwing an interface-typed operand is rejected
//! rule: An interface-typed operand is likewise rejected — no interface
//!   derives from `Exception` — even when the runtime class implementing it
//!   does; narrow to the exception class first.
//! cites: 31.1
//! expect: reject
//! diagnostic: interface
module Conf;

public interface IMarker
end IMarker;

public class BadException: Exception, IMarker
  public constructor(message: string) base(message)
  end constructor;
end BadException;

public class Program
  public static method MakeMarker(): owned IMarker => new BadException("bad");

  public static method Main()
    var m := MakeMarker();
    throw m;
  end Main;
end Program;
