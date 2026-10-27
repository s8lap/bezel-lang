//! case: s11_6_0016
//! doc: spec
//! section: 11.6
//! title: the same `where` clause names the nested type of a NON-generic enclosing type
//! rule: A nested type none of whose enclosing types is generic is referenced from outside as
//!   `Outer.Nested` (§11.6, v0.340) — with the enclosing class not generic, `C.N` is that
//!   spelling, and the same type position is legal.
//! cites: 11.6
//! expect: accept
module Conf;

public class C
  public interface IN
    method V(): int;
  end IN;
end C;

public class D
  public static method Use<U>(u: U): int where U: C.IN
    return 0;
  end Use;
end D;
