//! case: s10_2_0031
//! doc: spec
//! section: 10.2
//! title: same-valued enum constants draw no diagnostic
//! rule: Two or more constants of one enum may share an underlying value ...
//!   Aliasing is legal and **draws no diagnostic**; exhaustiveness is over the set of
//!   distinct declared *values*, so an arm naming either alias covers both.
//! cites: 10.2, 19.6
//! expect: quiet
module Conf;

public enum Tier
  Legacy := 1,
  Current := 1,
  Extended := 2;
end Tier;

public class Router
  public method Weight(t: Tier): int =>
      (case t of
        Legacy => 10,
        Extended => 20);
end Router;
