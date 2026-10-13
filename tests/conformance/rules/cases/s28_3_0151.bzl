//! case: s28_3_0151
//! doc: spec
//! section: 28.3
//! title: a receiver-sourced borrow assigned to a settable static property is rejected
//! rule: A store into a non-`owned` static field ... or into a static property not
//!   through `set(owned)` (the preceding bullet) of a class- or interface-typed
//!   borrow admits it only when it is static-storage-sourced ... A borrow of any other source — a plain or `scoped` parameter, `this`, a
//!   field, getter or indexer reached through the receiver, a capture, a local in
//!   any state, an `owned` parameter, a temporary, or the result of invoking a
//!   function value — is a compile error at the store
//! cites: 28.3, 22.12
//! expect: reject
//! diagnostic: static
//! issue: 5702
module Conf;


import System.Threading;

public class Gate
  public static property M: Mutex? := null;
end Gate;

public class Owner
  private owned _mine: Mutex;

  public constructor()
    _mine := new Mutex();
  end constructor;

  public method Publish()
    Gate.M := this._mine;
  end Publish;
end Owner;
