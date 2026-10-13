//! case: s5_1_0117
//! doc: spec
//! section: 5.1
//! title: an interface reference converts implicitly to an interface it transitively extends
//! rule: §5.1 (reference conversions): "interface to extended interface, directly or transitively".
//!   H extends J and J extends I; a K reference converts to I, and the call through I reaches K's P.
//! cites: 5.1
//! expect: run
//! stdout: 4
module Conf;

public interface I
  method P(): int;
end I;

public interface J: I
end J;

public interface H: J
end H;

public class K: H
  public constructor() end constructor;

  public method P(): int
    return 4;
  end P;
end K;

public class C
  public static method Main(): int
    var i: I := new K();
    Console.WriteLine(i.P());
    return 0;
  end Main;
end C;
