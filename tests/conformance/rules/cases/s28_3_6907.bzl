//! case: s28_3_6907
//! doc: spec
//! section: 28.3
//! title: a receiver-sourced Mutex read added to a collection is reported as a collection
//!   store
//! rule: the trust-admission report (§28.3) names every such store, and a `scoped`
//!   parameter (§28.14) makes the bound static.
//! cites: 28.3, 28.11, 28.13
//! expect: warn
//! diagnostic: borrowing-collection store
//! flags: --report-trust-admissions
//! issue: 7468
//! notes: The first sentence of the same bullet states the rule: a borrow may be stored in
//!   a field of an object only if the borrow's source provably outlives that object; clause
//!   3's residual is a K4.3-recorded trust clause, and the report mode names every such
//!   store.
module Conf;
import System.Threading;
public class Sink
  private owned _m: Mutex;
  private _kept: Mutex?;
  private owned _ms: Array<Mutex>;
  public constructor()
    _m := new Mutex();
    _ms := new Array<Mutex>();
  end constructor;
  public method Take()
    _kept := this._m;
    _ms.Add(this._m);
  end Take;
end Sink;
