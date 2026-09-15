//! case: l2_16_0005
//! doc: stdlib
//! section: 2.16
//! title: Shared<T> rejects a value-typed T at instantiation
//! rule: Shared<T> requires where T: class — proper class types only; a
//!   runtime-managed or value-typed T is rejected at instantiation.
//! cites: 2.16
//! expect: reject
//! diagnostic: class
module Conf;

public class Program
  public method Demo()
    using var bad := new Shared<int>(5);
  end Demo;
end Program;
