//! case: l4_2_0007
//! doc: stdlib
//! section: 4.2
//! title: OwningLinkedList<T> rejects a function-typed T at instantiation
//! rule: Runtime-managed reference types (string, function values) are
//!   rejected at the owning family's instantiation site, alongside
//!   value-typed elements — ownership only makes sense for class-typed
//!   values.
//! cites: 4.2, 14.4, 28.11
//! expect: reject
//! diagnostic: class
module Conf;

public class Program
  public method Demo()
    using var pool := new OwningLinkedList<(int) -> int>();
  end Demo;
end Program;
