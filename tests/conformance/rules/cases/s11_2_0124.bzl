//! case: s11_2_0124
//! doc: spec
//! section: 11.2
//! title: an interface extending a generic interface carries the substitution of its argument
//! rule: §11.2: "An interface may extend other interfaces." §11.2's substitution rule: "The interface's substitution maps each of the interface's type parameters to the implementing type's argument."
//!   J<T> extends I<T>, so under K: J<int> the inherited member P returns int, and the implementation's int return matches.
//! cites: 11.2, 5.1
//! expect: run
//! stdout: 3
module Conf;

public interface I<T>
  method P(): T;
end I;

public interface J<T>: I<T>
end J;

public class K: J<int>
  public constructor() end constructor;

  public method P(): int
    return 3;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K();
    Console.WriteLine(s.P());
    return 0;
  end Main;
end C;
