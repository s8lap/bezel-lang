//! case: s22_15_0002
//! doc: spec
//! section: 22.15
//! title: a pure body may not call a non-pure member
//! rule: The compiler rejects, inside a pure body, any call whose
//!   statically resolved declaration is not pure.
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
module Conf;

public class Box
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public pure method Get(): int => _v;

  public method Bump()
    _v := _v + 1;
  end Bump;
end Box;

public class Reader
  public static pure method Touch(b: Box): int
    b.Bump();
    return b.Get();
  end Touch;
end Reader;
