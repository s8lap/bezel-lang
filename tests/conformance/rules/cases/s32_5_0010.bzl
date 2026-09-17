//! case: s32_5_0010
//! doc: spec
//! section: 32.5
//! title: a nullable value type parameter is rejected
//! rule: Nullable value types are rejected: `int?`, `bool?`, `IntPtr?`, `Ptr<T>?`, and
//!   nullable record forms have no C representation.
//! cites: 32.5
//! expect: reject
//! diagnostic: nullable
module Conf;

extern "libexample"
  method MaybeCount(n: int?): int;
end;
