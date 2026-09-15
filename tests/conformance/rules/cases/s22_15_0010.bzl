//! case: s22_15_0010
//! doc: spec
//! section: 22.15
//! title: pure is rejected on an iterator method
//! rule: `pure` is rejected on iterator methods (`yield`) — a
//!   pure-iterator form is deliberately left open, not designed.
//! cites: 22.15
//! expect: reject
//! diagnostic: yield
module Conf;

public class Nums
  public static pure method All(): IEnumerable<int>
    yield return 1;
  end All;
end Nums;
