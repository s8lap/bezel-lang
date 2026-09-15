//! case: s25_1_0003
//! doc: spec
//! section: 25.1
//! title: the null-coalescing operator is not overloadable
//! rule: The null-forgiving `!`, the null-coalescing `??`, and the type-test
//!   operators have null-aware or type-system semantics fixed by the language;
//!   user types do not customize them.
//! cites: 25.1
//! expect: reject
//! diagnostic: overload
module Conf;

public record Maybe(Value: int)
  public static method operator ??(a: Maybe, b: Maybe): Maybe => a;
end Maybe;
