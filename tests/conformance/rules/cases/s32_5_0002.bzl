//! case: s32_5_0002
//! doc: spec
//! section: 32.5
//! title: Ptr<T> is admitted as a parameter, a return type, and inside an FFI function type
//! rule: `Ptr<T>` -- a typed foreign address; admitted as a parameter, as a return
//!   type, and inside FFI function types, marshalling as the pointer it is.
//! cites: 32.5, 32.12
//! expect: accept
module Conf;
import System.Interop;

extern "libexample"
  method FindFirst(data: Ptr<int>, count: int,
      pred: (Ptr<int>) -> bool): Ptr<int>;
end;
