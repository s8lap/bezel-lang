//! case: s11_2_0010
//! doc: spec
//! section: 11.2
//! title: a swapped own type parameter in an owned generic return type is rejected
//! rule: Return types must match exactly under the interface's substitution, and a member's own type parameters correspond by number and position. Interface P<T, U>(a: T): owned Box<T> is not matched by P<A, B>(a: A): owned Box<B>, because the returned Box's argument is position 1 on the interface and position 2 on the implementation.
//! cites: 11.2
//! expect: reject
//! diagnostic: return type
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<T, U>(a: T): owned Box<T>;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: A): owned Box<B>
    return new Box<B>();
  end P;
end K;
