//! case: s28_3_0152
//! doc: spec
//! section: 28.3
//! title: a method result reached through a static's referent is not static storage
//! rule: A borrow reached through such a read — a field, getter, indexer or method
//!   result of the referent (`T.f.g`, `T.P.Peek()`) — is not static storage: its
//!   source is the referent's own state, which a store or drop-on-overwrite
//!   (§28.7) can replace
//! cites: 28.3, 22.12
//! expect: reject
//! diagnostic: static
//! issue: 5702
module Conf;


import System.Threading;

public class Pool
  private owned _m: Mutex;

  public constructor()
    _m := new Mutex();
  end constructor;

  public method Get(): Mutex => _m;
end Pool;

public class Holder
  public static owned readonly Shared: Pool := new Pool();
end Holder;

public class Gate
  private static _m: Mutex? := null;

  public static method Install()
    _m := Holder.Shared.Get();
  end Install;
end Gate;
