//! case: s11_3_0011
//! doc: spec
//! section: 11.3
//! title: methods with the same type-parameter count whose parameter types are permuted do not clash
//! rule: "`M<T, U>(a: T, b: U)` and `M<T, U>(a: U, b: T)` do not clash: an
//!   explicit type-argument list naming two different types gives them different
//!   signatures, and a call that leaves them to inference is ambiguous.
//!   Two methods declaring the same number of type parameters compare their
//!   parameter types with those type parameters corresponding by position, not by
//!   name. The parameter types do not correspond positionally, so the pair is
//!   distinguishable."
//! cites: 11.3, 14.5
//! expect: accept
module Conf;

public class Sample
  public constructor()
  end constructor;

  public method Pair<T, U>(a: T, b: U): string => "normal";
  public method Pair<T, U>(a: U, b: T): string => "permute";
end Sample;
