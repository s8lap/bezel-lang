//! case: s32_13_0003
//! doc: spec
//! section: 32.13
//! title: a single-field record over an integer scalar is admitted directly by value
//! rule: Typed handles: the single-field admission. A C-compatible record with exactly
//!   one field, of integer or pointer scalar type, is FFI-compatible directly --
//!   parameter, return, `ref`/`out`, and callback positions -- marshalling exactly
//!   as its field.
//! cites: 32.13
//! expect: accept
module Conf;

public record Sqlite3Db(Raw: IntPtr);

extern "sqlite3"
  method sqlite3_open(filename: CString, out db: Sqlite3Db): int;
  method sqlite3_close(db: Sqlite3Db): int;
end;
