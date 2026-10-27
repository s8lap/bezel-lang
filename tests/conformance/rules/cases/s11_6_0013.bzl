//! case: s11_6_0013
//! doc: spec
//! section: 11.6
//! title: a nested type of a generic type is not named as a generic type argument
//! rule: The bare-type-name row of §2.7's normative table: a name the grammar reads only as a `Type` —
//!   an ascription, `new`, a generic argument, a base or interface list, the right operand of `is`,
//!   `is not` or `as`, a `typeof` or `default` operand, a destructuring pattern's head, a `where`
//!   clause — reads a type position, and §11.6's "no name passes through a generic enclosing
//!   type" makes the bare generic segment in `C.N` an Appendix C.1 arity error (§2.7, §11.6,
//!   v0.340).
//! cites: 11.6, 2.7, 2.2
//! expect: reject
//! diagnostic: generic-argument count
module Conf;

public class Box<U>
  public f: int := 0;
end Box;

public class C<T>
  public class N
    public method V(): int => 7;
  end N;
end C;

public class D
  public static method Take(b: Box<C.N>): int => 0;
end D;
