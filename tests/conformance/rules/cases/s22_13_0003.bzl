//! case: s22_13_0003
//! doc: spec
//! section: 22.13
//! title: a forward read routed through a statically-resolvable call is rejected
//! rule: ...directly, or transitively through the bodies of
//!   statically-resolvable calls (static methods and static property
//!   getters reached by direct name within the compilation unit) — the
//!   reference is rejected at compile time.
//! cites: 22.13
//! expect: reject
//! diagnostic: forward reference
module Conf;

public class Config
  private static readonly _a: int := Helper();
  private static readonly _b: int := 5;

  private static method Helper(): int => _b;
end Config;
