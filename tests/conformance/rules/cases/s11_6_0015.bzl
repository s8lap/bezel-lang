//! case: s11_6_0015
//! doc: spec
//! section: 11.6
//! title: a nested type of a generic type is not named in a `where` clause
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
  public interface IN
    method V(): int;
  end IN;
end C;

public class D
  public static method Use<U>(u: U): int where U: C.IN
    return 0;
  end Use;
end D;
