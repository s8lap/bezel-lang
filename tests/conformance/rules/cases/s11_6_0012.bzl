//! case: s11_6_0012
//! doc: spec
//! section: 11.6
//! title: the same `as` operand names the nested type of a NON-generic enclosing type
//! rule: A nested type none of whose enclosing types is generic is referenced from outside as
//!   `Outer.Nested` (§11.6, v0.340) — with the enclosing class not generic, `C.N` is that
//!   spelling, and the same type position is legal.
//! cites: 11.6
//! expect: accept
module Conf;

public class C
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
