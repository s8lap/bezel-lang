//! case: s32_11_0001
//! doc: spec
//! section: 32.11
//! title: the symbol alias clause binds a declaration to a different symbol name
//! rule: The alias clause `:= symbol "name"`, written after the return clause, binds
//!   the declaration to the named symbol instead.
//! cites: 32.11
//! expect: accept
module Conf;

extern "libexample"
  method GetVersion(): int := symbol "example_get_version";
end;
