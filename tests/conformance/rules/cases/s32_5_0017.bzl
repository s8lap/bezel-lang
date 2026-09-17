//! case: s32_5_0017
//! doc: spec
//! section: 32.5
//! title: scoped is illegal on an extern parameter
//! rule: `scoped` function-typed parameters are illegal on `extern` declarations:
//!   neither the storage obligations nor the quiescence guarantee can be verified
//!   across the FFI boundary.
//! cites: 32, 32.5, 28.14
//! expect: reject
//! diagnostic: scoped
module Conf;

extern "libexample"
  method Subscribe(scoped handler: (int) -> ()): int;
end;
