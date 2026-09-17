//! case: s32_3_0002
//! doc: spec
//! section: 32.3
//! title: a full path library name bypasses runtime resolution
//! rule: For libraries with non-conventional names, a full path may be used as the
//!   library name; this bypasses runtime resolution and loads the file directly.
//! cites: 32.3
//! expect: accept
module Conf;

extern "/opt/myapp/lib/libcustom.so.2.1"
  public method DoWork(): int;
end;
