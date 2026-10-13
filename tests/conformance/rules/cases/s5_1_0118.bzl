//! case: s5_1_0118
//! doc: spec
//! section: 5.1
//! title: a class does not convert implicitly to an interface it does not implement
//! rule: §5.1 (reference conversions) lists the implicit conversion "a type to an interface it implements or conforms to".
//!   K declares Q, not the interface member P, so K does not implement I and the conversion of a K reference to I is not among the listed implicit conversions.
//! cites: 5.1, 11.2
//! expect: reject
//! diagnostic: cannot convert
module Conf;

public interface I
  method P(): int;
end I;

public class K
  public constructor() end constructor;

  public method Q(): int
    return 1;
  end Q;
end K;

public class C
  public static method Main(): int
    var i: I := new K();
    return 0;
  end Main;
end C;
