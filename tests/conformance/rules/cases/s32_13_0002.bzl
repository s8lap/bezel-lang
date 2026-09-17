//! case: s32_13_0002
//! doc: spec
//! section: 32.13
//! title: a record with a string field is not C-compatible and rejected as a ref parameter
//! rule: Everything else disqualifies: `string`, `char`, `decimal`, nullable types,
//!   enums, class/interface/function-typed fields.
//! cites: 32.13
//! expect: reject
//! diagnostic: c-compatible
module Conf;

public record Labeled(Name: string, Value: int);

extern "libexample"
  method Describe(ref item: Labeled): int;
end;
