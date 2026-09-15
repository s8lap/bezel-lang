//! case: s32_6_0001
//! doc: spec
//! section: 32.6
//! title: CString cannot be constructed directly with new
//! rule: `CString`, `WString`, and `RawBytes` are FFI-boundary types. They cannot be
//!   constructed or manipulated directly in ordinary Bezel code; the standard
//!   library provides explicit conversion functions.
//! cites: 32.6
//! expect: reject
//! diagnostic: constructor
module Conf;

public static class Strings
  public static method MakeBad()
    using var c := new CString();
  end MakeBad;
end Strings;
