//! case: s10_4_0031
//! doc: spec
//! section: 10.4
//! title: an enum's static owned readonly FrozenArray singleton is warning-free
//! rule: `FrozenArray<Priority>` declares the **immutable** statics-and-sharing fact,
//!   placing it in §22.12 rule 4's warning-suppression set — a `static owned readonly`
//!   of it is a warning-free, every-domain-readable singleton.
//! cites: 10.4, 22.12, 22.13
//! expect: quiet
module Conf;

public enum Priority
  Low, Medium, High;

  public static owned readonly All: FrozenArray<Priority> :=
      FrozenArray<Priority>.From([Low, Medium, High]);

  public static method Size(): int
    return Priority.All.Length;
  end Size;
end Priority;
