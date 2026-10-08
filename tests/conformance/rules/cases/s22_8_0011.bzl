//! case: s22_8_0011
//! doc: spec
//! section: 22.8
//! title: a readonly field assigned inside a lambda written in a constructor body is rejected
//! rule: **The window is lexical and does not include a nested function
//!   body**: the body of a lambda (§27.3) written inside a constructor body,
//!   a field initializer, a static constructor or a static field initializer
//!   is outside every assignment window ... so an assignment to a `readonly`
//!   field inside a lambda ... is a compile error wherever the lambda stands
//! cites: 22.8, 27.3
//! expect: reject
//! diagnostic: readonly
//! issue: 5651
module Conf;

public class Gauge
  private readonly _level: int;

  public constructor()
    var setLevel := lambda() => do
      _level := 5;
    end;
    setLevel();
  end constructor;

  public method Level(): int => _level;
end Gauge;
