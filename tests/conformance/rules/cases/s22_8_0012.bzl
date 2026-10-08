//! case: s22_8_0012
//! doc: spec
//! section: 22.8
//! title: a static readonly field assigned inside a lambda written in the static constructor is rejected
//! rule: **The window is lexical and does not include a nested function
//!   body**: the body of a lambda (§27.3) written inside a constructor body,
//!   a field initializer, a static constructor or a static field initializer
//!   is outside every assignment window ... so an assignment to a `readonly`
//!   field inside a lambda ... is a compile error wherever the lambda stands
//! cites: 22.8, 27.3, 22.13
//! expect: reject
//! diagnostic: readonly
//! issue: 5651
module Conf;

public class Registry
  private static readonly _capacity: int;

  static constructor()
    var init := lambda() => do
      _capacity := 64;
    end;
    init();
  end constructor;

  public static method Capacity(): int => _capacity;
end Registry;
