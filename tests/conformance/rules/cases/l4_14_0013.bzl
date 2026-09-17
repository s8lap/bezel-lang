//! case: l4_14_0013
//! doc: stdlib
//! section: 4.14
//! title: OwningBuffer<T> does not admit a runtime-managed reference type such as string
//! rule: What is not admitted. Runtime-managed reference types (string,
//!   function types) are not admitted as T -- they have no disposal
//!   obligation, so the owning semantics add nothing.
//! cites: 4.14
//! expect: reject
//! diagnostic: not admitted
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<string>(1);
  end Main;
end Program;
