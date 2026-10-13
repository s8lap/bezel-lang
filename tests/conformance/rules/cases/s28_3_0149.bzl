//! case: s28_3_0149
//! doc: spec
//! section: 28.3
//! title: a plain parameter stored into a mutable static Mutex field is rejected
//! rule: A borrow of any other source — a plain or `scoped` parameter, `this`, a
//!   field, getter or indexer reached through the receiver, a capture, a local in
//!   any state, an `owned` parameter, a temporary, or the result of invoking a
//!   function value — is a compile error at the store ... `_m := m;` with `m` a plain parameter is rejected
//! cites: 28.3, 22.12
//! expect: reject
//! diagnostic: static
//! issue: 5702
module Conf;


import System.Threading;

public class Gate
  private static _m: Mutex? := null;

  public static method Install(m: Mutex)
    _m := m;
  end Install;
end Gate;
