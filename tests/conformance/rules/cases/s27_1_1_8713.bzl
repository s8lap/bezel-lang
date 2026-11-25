//! case: s27_1_1_8713
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value of a class returned at an owned interface return slot is
//!   admitted
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5, 14.2
//! expect: accept
//! notes: Twin of s27_1_1_8712: the return type is `() -> owned IShape`, so the modifier slots
//!   match and the class-to-interface covariance of the return slot applies.
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
  public static method Make(): () -> owned IShape
    var f: () -> owned Sq := lambda() => new Sq(3);
    return f;
  end Make;
end Program;
