//! case: s11_6_0011
//! doc: spec
//! section: 11.6
//! title: a nested type of a generic type is not named at the right operand of `as`
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

public class C<T>
  public class N
    public method V(): int => 7;
  end N;

  public static method Make(): owned N => new N();
end C;

public class D
  public static method Cast(n: Object): Object?
    return n as C.N;
  end Cast;
end D;
