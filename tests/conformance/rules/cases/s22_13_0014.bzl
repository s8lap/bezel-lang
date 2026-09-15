//! case: s22_13_0014
//! doc: spec
//! section: 22.13
//! title: a static readonly field of a non-trivial-dispose class declared without owned warns
//! rule: The compiler warns (not errors) when a `static readonly` field
//!   of a class type whose `Dispose()` is non-trivial — a class with a
//!   user-defined destructor ... — is declared without `owned`.
//! cites: 22.13, 28.9
//! expect: warn
//! diagnostic: Dispose
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
  end destructor;
end Resource;

public class Registry
  private static readonly _r: Resource := new Resource();
end Registry;
