//! case: s5_1_0119
//! doc: spec
//! section: 5.1
//! title: a class that implements the interface converts to it: the twin
//! rule: §5.1 (reference conversions): "a type to an interface it implements or conforms to". K declares P, so K implements I and the conversion is implicit.
//! cites: 5.1, 11.2
//! expect: run
//! stdout: 1
module Conf;

public interface I
  method P(): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(): int
    return 1;
  end P;
end K;

public class C
  public static method Main(): int
    var i: I := new K();
    Console.WriteLine(i.P());
    return 0;
  end Main;
end C;
