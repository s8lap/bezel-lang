//! case: s22_10_0017
//! doc: spec
//! section: 22.10
//! title: scoped on a value-typed primary-constructor parameter is rejected
//! rule: A primary-constructor parameter list admits plain, `owned`, and
//!   class- or interface-typed `scoped` parameters. `scoped` is valid on
//!   function-typed parameters and on class/interface-typed parameters;
//!   other parameter types are rejected semantically (Appendix A.4).
//! cites: 22.10, 28.14, 22.3
//! expect: reject
//! diagnostic: scoped
module Conf;

public class Box(scoped n: int)
  public method Get(): int => n;
end Box;
