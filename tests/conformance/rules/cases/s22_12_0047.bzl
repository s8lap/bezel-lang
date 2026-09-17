//! case: s22_12_0047
//! doc: spec
//! section: 22.12
//! title: a static owned readonly Locked registry is the canonical warning-free process-global
//! rule: The four members outside rule 2's form (i) — the data-owning locks
//!   `Locked<T>`/`RwLocked<T>` (`static owned readonly` being the canonical
//!   process-global registry idiom) ... — are in rule 4's warning-suppression set,
//!   so the declaration is admitted warning-free.
//! cites: 22.12, 30.12, 22.13
//! expect: quiet
module Conf;

import System.Threading;

public sealed class Tally
  private _n: int := 0;

  public constructor()
  end constructor;

  public method Bump()
    _n := _n + 1;
  end Bump;
end Tally;

public static class Counters
  private static owned readonly _tally: Locked<Tally> :=
      new Locked<Tally>(new Tally());

  public static method Bump()
    using var g := _tally.Lock();
    g.Value.Bump();
  end Bump;
end Counters;
