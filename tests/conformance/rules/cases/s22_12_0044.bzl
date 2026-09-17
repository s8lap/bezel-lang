//! case: s22_12_0044
//! doc: spec
//! section: 22.12
//! title: a static owned readonly FrozenArray is silent by its immutable fact
//! rule: The warning-suppression set is derived, not curated: every type declaring
//!   the **synchronized** or the **immutable** fact — `FrozenArray<T>` among them —
//!   so a `static owned readonly` of it is admitted warning-free.
//! cites: 22.12, 30.14
//! expect: quiet
module Conf;

public static class Levels
  public static owned readonly All: FrozenArray<int> :=
      FrozenArray<int>.From([1, 2, 3]);

  public static method Size(): int
    return Levels.All.Length;
  end Size;
end Levels;
