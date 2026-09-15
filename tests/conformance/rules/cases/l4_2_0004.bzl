//! case: l4_2_0004
//! doc: stdlib
//! section: 4.2
//! title: OwningArray<string> is rejected at the instantiation site
//! rule: The where T: class constraint is enforced at the instantiation
//!   site: a runtime-managed reference type such as string has no
//!   disposal obligation and is rejected where the owning family requires
//!   a class or interface element type.
//! cites: 4.2, 14.4, 28.11
//! expect: reject
//! diagnostic: class
module Conf;

public class Program
  public method Demo()
    using var pool := new OwningArray<string>();
  end Demo;
end Program;
