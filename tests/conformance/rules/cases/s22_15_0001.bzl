//! case: s22_15_0001
//! doc: spec
//! section: 22.15
//! title: a pure method may call another pure member
//! rule: The compiler rejects, inside a pure body, any call whose
//!   statically resolved declaration is not pure — a call to a pure
//!   member is therefore admitted.
//! cites: 22.15
//! expect: accept
module Conf;

public class Box
  private _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public pure method Get(): int => _v;
end Box;

public class Reader
  public static pure method Describe(b: Box): int => b.Get();
end Reader;
