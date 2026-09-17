//! case: s22_10_0009
//! doc: spec
//! section: 22.10
//! title: an owned primary-constructor parameter referenced in a body position is captured as an owned field
//! rule: Referenced in a body position: captured as a compiler-generated
//!   owned private field, disposed by the generated `Dispose()` exactly
//!   like a declared owned field.
//! cites: 22.10, 28.4
//! expect: accept
module Conf;

import System.IO;

public class Holder(owned resource: Stream)
  public method Touch()
    resource.Flush();
  end Touch;
end Holder;
