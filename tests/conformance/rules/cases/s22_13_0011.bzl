//! case: s22_13_0011
//! doc: spec
//! section: 22.13
//! title: await is rejected in a static constructor body
//! rule: `await` may not appear in static field initializers or static
//!   constructor bodies.
//! cites: 22.13
//! expect: reject
//! diagnostic: await
module Conf;

public class Config
  static constructor()
    var v := await Task.FromResult(1);
  end constructor;
end Config;
