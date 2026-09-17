//! case: s32_5_0005
//! doc: spec
//! section: 32.5
//! title: the nullable boundary forms CString?, WString?, RawBytes? and a nullable function type are admitted
//! rule: Nullable boundary forms: `CString?`, `WString?`, `RawBytes?`, and nullable FFI
//!   function types are admitted as possibly-NULL pointers.
//! cites: 32.5
//! expect: accept
module Conf;

extern "libexample"
  method MaybeGreet(name: CString?): int;
  method MaybeTitle(title: WString?): int;
  method MaybeSend(payload: RawBytes?): int;
  method Configure(hook: ((int) -> ())?): int;
end;
