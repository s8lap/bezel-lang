//! case: s23_2_0004
//! doc: spec
//! section: 23.2
//! title: readonly on a block form declaring a setter is rejected
//! rule: On a block form declaring a setter `readonly` is contradictory and is
//!   likewise an error.
//! cites: 23.2, 23.3
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Counter
  private _n: int := 0;

  public constructor()
  end constructor;

  public readonly property Value: int
    get => _n;
    set => do _n := value; end;
  end Value;
end Counter;
