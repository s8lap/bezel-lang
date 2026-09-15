//! case: s22_13_0010
//! doc: spec
//! section: 22.13
//! title: await is rejected in a static field initializer
//! rule: `await` may not appear in static field initializers or static
//!   constructor bodies.
//! cites: 22.13
//! expect: reject
//! diagnostic: await
module Conf;

public class Config
  private static _v: int := await Task.FromResult(1);
end Config;
