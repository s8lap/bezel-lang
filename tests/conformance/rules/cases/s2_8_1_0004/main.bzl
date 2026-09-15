//! case: s2_8_1_0004
//! doc: spec
//! section: 2.8.1
//! title: the root compilation may not declare a dependency's module name
//! rule: A build whose artifact set — the root compilation plus the
//!   dependency closure — contains two distinct artifacts declaring the same
//!   module name, or a dependency artifact declaring a module name the root
//!   compilation also declares, is ill-formed; the diagnostic names both
//!   origins.
//! cites: 2.8.1, C.13
//! expect: reject
//! diagnostic: Lib
module Lib;

public class Bar
  public constructor()
  end constructor;
end Bar;
