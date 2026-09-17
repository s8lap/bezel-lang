//! case: s22_10_0008
//! doc: spec
//! section: 22.10
//! title: an owned primary-constructor parameter referenced only in an initializer is not captured
//! rule: Referenced only in field initializers (or the base call): not
//!   captured — the initializer consumes it.
//! cites: 22.10
//! expect: accept
module Conf;

import System.IO;

public class ResourceHolder(owned resource: Stream)
  private owned _resource: Stream := resource;
end ResourceHolder;
