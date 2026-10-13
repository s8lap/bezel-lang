//! case: s11_2_0130
//! doc: spec
//! section: 11.2
//! title: a generic method keeping the constraint set matches: the accepting twin
//! rule: §11.2: "and, for a generic method, a constraint set equivalent to the interface method's (§14.2)." Both sides here carry where V: new() / U: new().
//! cites: 11.2, 14.2
//! expect: accept
module Conf;

public interface I
  method Count<U>(): int where U: new();
end I;

public class K: I
  public constructor() end constructor;

  public method Count<V>(): int where V: new()
    return 1;
  end Count;
end K;
