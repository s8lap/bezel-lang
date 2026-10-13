//! case: s11_2_0126
//! doc: spec
//! section: 11.2
//! title: an interface extending a generic interface at a concrete argument is matched by the implementation
//! rule: §11.2: "An interface may extend other interfaces." Here J extends I<string> with a concrete argument, so its inherited P returns string.
//!   The implementation's return type string is exactly equal under that substitution.
//! cites: 11.2, 5.1
//! expect: run
//! stdout: hi
module Conf;

public interface I<T>
  method P(): T;
end I;

public interface J: I<string>
end J;

public class K: J
  public constructor() end constructor;

  public method P(): string
    return "hi";
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<string> := new K();
    Console.WriteLine(s.P());
    return 0;
  end Main;
end C;
