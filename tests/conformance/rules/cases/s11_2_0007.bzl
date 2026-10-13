//! case: s11_2_0007
//! doc: spec
//! section: 11.2
//! title: own type parameters inside a generic parameter type match by position, not by name
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters take part by number and position, not by name. Interface P<T, U>(a: Box<T>) is not matched by P<A, B>(a: Box<B>): under the position correspondence T to A and U to B, the interface's parameter is Box<A> and the implementation's is Box<B>.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<T, U>(a: Box<T>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Box<B>): int
    return 1;
  end P;
end K;
