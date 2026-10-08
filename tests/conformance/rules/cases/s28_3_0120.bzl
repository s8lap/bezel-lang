//! case: s28_3_0120
//! doc: spec
//! section: 28.3
//! title: a function-typed out location carries the empty set after the call and may be a field
//! rule: except at a location whose type, after substitution, is a function type:
//!   the callee can store there only a capture-free function value ..., so
//!   the call's borrows need no admissibility check against it, and on the
//!   call's normal return an `out` location carries the empty set and a `ref`
//!   location the set it carried before the call
//! cites: 28.3, 28.14
//! expect: accept
module Conf;

public class Cache
  private _g: (() -> int)?;

  public constructor()
  end constructor;

  public method Load(): bool
    using var d := new Dictionary<string, () -> int>();
    return d.TryGetValue("k", out this._g);
  end Load;
end Cache;
