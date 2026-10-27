//! case: s22_16_0018
//! doc: spec
//! section: 22.16
//! title: an undischargeable call to a static requires member is a compile error at the call
//! rule: The `static` clause qualifier demands the proven tier: a
//!   `static` clause the implementation cannot discharge statically is a
//!   compile error.
//! cites: 22.16
//! expect: reject
//! diagnostic: proven
module Conf;

public class Ops
  public static method Check(x: int): bool
    static requires(x > 0)

    return true;
  end Check;

  public static method Seven(): int => 7;

  public static method Use(): bool
    return Ops.Check(Ops.Seven());
  end Use;
end Ops;
