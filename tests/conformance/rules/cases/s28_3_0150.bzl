//! case: s28_3_0150
//! doc: spec
//! section: 28.3
//! title: a plain Task parameter stored into a lazy task cache static is rejected
//! rule: A store into a non-`owned` static field — a mutable static §22.12 rule 2
//!   admits ... of a class- or interface-typed borrow admits it only when it
//!   is static-storage-sourced ... A borrow of any other source — a plain or `scoped` parameter, `this`, a
//!   field, getter or indexer reached through the receiver, a capture, a local in
//!   any state, an `owned` parameter, a temporary, or the result of invoking a
//!   function value — is a compile error at the store
//! cites: 28.3, 22.12, 22.13
//! expect: reject
//! diagnostic: static
//! issue: 5702
module Conf;


public class Config
  public constructor()
  end constructor;
end Config;

public class Cache
  private static _cfg: Task<Config>? := null;

  public static method Prime(t: Task<Config>)
    _cfg := t;
  end Prime;
end Cache;
