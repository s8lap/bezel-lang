//! case: s22_12_0014
//! doc: spec
//! section: 22.12
//! title: an OwningTask-typed static field is not admitted as a lazy task cache
//! rule: `OwningTask<T>?` — mutable: error — not (ii), †.
//! cites: 22.12
//! expect: reject
//! diagnostic: OwningTask
module Conf;

public class AppConfig
  private static _cfg: OwningTask<int>? := null;
end AppConfig;
