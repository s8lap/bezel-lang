//! case: s23_2_0003
//! doc: spec
//! section: 23.2
//! title: readonly on a get-only block-form property is rejected
//! rule: A get-only block form expresses read-onlyness by its shape; `readonly`
//!   there is a compile error.
//! cites: 23.2, 23.3
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Counter
  private _n: int := 0;

  public constructor()
  end constructor;

  public readonly property Doubled: int
    get => _n * 2;
  end Doubled;
end Counter;
