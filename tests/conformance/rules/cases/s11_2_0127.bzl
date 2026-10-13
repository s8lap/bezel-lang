//! case: s11_2_0127
//! doc: spec
//! section: 11.2
//! title: a diamond of extended interfaces is satisfied by one implementation of the shared member
//! rule: §11.2: "Diamond resolution: a single implementation in the implementing class satisfies all interface methods with the same signature."
//!   J<T> extends H<T> and I<T>, and H<T> extends I<T>, so P is reached through two paths; both substitute to the same signature, and one P returning 6 satisfies both.
//! cites: 11.2
//! expect: run
//! stdout: 6
module Conf;

public interface I<T>
  method P(): T;
end I;

public interface H<T>: I<T>
end H;

public interface J<T>: H<T>, I<T>
end J;

public class K: J<int>
  public constructor() end constructor;

  public method P(): int
    return 6;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K();
    Console.WriteLine(s.P());
    return 0;
  end Main;
end C;
