//! case: s22_12_0013
//! doc: spec
//! section: 22.12
//! title: a Task-typed lazy cache static field is admitted
//! rule: Lazy task cache: a static whose declared type is `Task?` or
//!   `Task<T>?` — the admission is keyed to the declared type alone.
//! cites: 22.12
//! expect: accept
module Conf;

public class AppConfig
  private static _cfg: Task<int>? := null;
end AppConfig;
