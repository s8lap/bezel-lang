//! case: s27_1_1_8712
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value of a class returned at a plain interface return slot is
//!   rejected
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5, 14.2
//! expect: reject
//! diagnostic: modifier slots
//! notes: Modifier slots must match exactly: `() -> owned Sq` is covariant to `() -> IShape` in the
//!   class slot, but its `owned` modifier is not the plain slot's. Rejected at the `return`.
//!   Twin: s27_1_1_8713.
module Conf;
public interface IShape
  method Area(): int;
end IShape;
public class Sq: IShape
  public readonly S: int;
  public constructor(s: int) S := s; end constructor;
  public method Area(): int => S * S;
end Sq;

public class Program
  public static method Make(): () -> IShape
    var f: () -> owned Sq := lambda() => new Sq(3);
    return f;
  end Make;
  public static method Main(): int => 0;
end Program;
