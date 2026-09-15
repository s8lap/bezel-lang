//! case: s22_12_0041
//! doc: spec
//! section: 22.12
//! title: a static owned readonly Mutex is in the warning-suppression set and is silent
//! rule: A `static readonly` (including `owned`) field of class type is admitted
//!   with a declaration-site warning **unless the class is in this rule's
//!   warning-suppression set** — every type declaring the **synchronized** or the
//!   **immutable** fact, `Mutex` among them.
//! cites: 22.12, 30.12
//! expect: quiet
module Conf;

import System.Threading;

public static class Gates
  private static owned readonly _gate: Mutex := new Mutex();

  public static method Enter(): owned Lock
    return _gate.Acquire();
  end Enter;
end Gates;
