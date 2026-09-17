//! case: s14_5_0008
//! doc: spec
//! section: 14.5
//! title: a type parameter appearing only in the return type is unbindable without a target
//! rule: A type parameter still unbound after arguments and lambdas is bound
//!   from the call's target type where one exists; a type parameter that
//!   remains unbound — appearing in no self-determined argument position and
//!   no target context — is a compile error naming the parameter (§14.5).
//! cites: 14.5
//! expect: reject
//! diagnostic: target type
module Conf;

public static class Util
  public static method Empty<T>(): owned Array<T> => new Array<T>();
end Util;

public class Program
  public static method Main()
    var loose := Util.Empty();
  end Main;
end Program;
