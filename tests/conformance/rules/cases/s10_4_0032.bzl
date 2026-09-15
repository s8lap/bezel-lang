//! case: s10_4_0032
//! doc: spec
//! section: 10.4
//! title: the same enum static at a mutable class type draws rule 4's sharing warning
//! rule: ... where the same field at `Array<Priority>` (a mutable class type outside
//!   the set) would draw rule 4's declaration-site sharing warning.
//! cites: 10.4, 22.12, 30.14
//! expect: warn
//! diagnostic: one domain
module Conf;

public enum Priority
  Low, Medium, High;

  public static owned readonly All: Array<Priority> := new Array<Priority>(3, Low);

  public static method Size(): int
    return Priority.All.Count;
  end Size;
end Priority;
