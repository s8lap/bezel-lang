//! case: s26_5_0053
//! doc: spec
//! section: 26.5
//! title: a Wait extension taking an argument is not a fallback for a Task instance call
//! rule: the call is a compile error citing the instance member(s); it does not fall
//!   back to an extension
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: instance member
module Conf;

extension Task<T>
  public method Wait(k: int)
  end Wait;
end extension;

public class Program
  public static method Main(): int
    using var t0 := Task.FromResult(2);
    var t: Task<int>? := t0;
    t?.Wait(3);
    return 0;
  end Main;
end Program;
