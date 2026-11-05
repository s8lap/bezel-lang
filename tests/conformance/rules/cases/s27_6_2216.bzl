//! case: s27_6_2216
//! doc: spec
//! section: 27.6
//! title: a method-group reference with no type-argument list and only
//!   generic overloads is rejected
//! rule: a reference to a generic method with no list is a compile error
//!   naming the type parameters to supply, and no inference binds them
//!   from the target
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: type parameter
//! notes: Exactly 1 error on the staging tip, all the quoted one; the
//!   accepting twin (the reference replaced by a call, or the
//!   ambiguity removed) compiles.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len<T, U>(x: T, y: U): int => 8;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f := v.Len;
  end Main;
end Program;
