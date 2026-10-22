//! case: s21_10_0028
//! doc: spec
//! section: 21.10
//! title: a local record that names an interface but omits one of its members is rejected
//! rule: The local type is an ordinary nominal record or enum — every §9 (records)
//!   and §10 (enums) rule applies unchanged; interface implementation (§9.6),
//!   and the derived type properties ... are all computed by their existing
//!   rules from the declaration.
//! cites: 21.10, 9.6, 12.5
//! expect: reject
//! diagnostic: Perim
module Conf;

public interface IShape
  method Area(): int;
  method Perim(): int;
end IShape;

public static class Util
  public static method Go(): int
    record Sq(S: int): IShape
      public method Area(): int => S * S;
    end Sq;
    var s: IShape := new Sq(3);
    return s.Area();
  end Go;
end Util;
