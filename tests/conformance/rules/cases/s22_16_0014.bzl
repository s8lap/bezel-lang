//! case: s22_16_0014
//! doc: spec
//! section: 22.16
//! title: old(old(E)) is a compile error
//! rule: `old(old(E))` is a compile error.
//! cites: 22.16
//! expect: reject
//! diagnostic: old
module Conf;

public class Ops
  public static method Weird(x: int): int
    ensures(old(old(x)) >= 0)

    return x;
  end Weird;
end Ops;
