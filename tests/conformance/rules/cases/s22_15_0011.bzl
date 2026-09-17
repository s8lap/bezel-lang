//! case: s22_15_0011
//! doc: spec
//! section: 22.15
//! title: pure is rejected on a destructor
//! rule: `pure` is rejected on destructors — disposal is the effect
//!   context purity fences off.
//! cites: 22.15
//! expect: reject
//! diagnostic: destructor
module Conf;

public class Res
  public constructor()
  end constructor;

  pure destructor
  end destructor;
end Res;
