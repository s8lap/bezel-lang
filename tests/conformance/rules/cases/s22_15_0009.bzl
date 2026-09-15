//! case: s22_15_0009
//! doc: spec
//! section: 22.15
//! title: pure is rejected on an async member
//! rule: `pure` is rejected on async members and async lambdas: an
//!   `async` body interleaves with the domain's continuation queue and
//!   traffics in tasks; `pure` implies non-`async`.
//! cites: 22.15
//! expect: reject
//! diagnostic: async
module Conf;

public class Ops
  public static pure async method Compute(): Task<int>
    return 1;
  end Compute;
end Ops;
