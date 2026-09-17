//! case: s22_15_0027
//! doc: spec
//! section: 22.15
//! title: pure is rejected on a set accessor
//! rule: Purity attaches to the getter; `pure` on `set` is always
//!   rejected.
//! cites: 22.15
//! expect: reject
//! diagnostic: pure set
module Conf;

public class Box3
  private _v: int := 0;

  public property Value: int
    get => _v;
    pure set => _v := value;
  end Value;
end Box3;
