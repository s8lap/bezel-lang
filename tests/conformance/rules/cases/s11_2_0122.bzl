//! case: s11_2_0122
//! doc: spec
//! section: 11.2
//! title: the non-nullable return matches the interface's int return, the accepting twin of the nullable form
//! rule: §11.2: "Return types must match exactly (no covariance in this version)." Both sides declare int here.
//! cites: 11.2
//! expect: accept
module Conf;

public interface I
  method Find(): int;
end I;

public class K: I
  public constructor() end constructor;

  public method Find(): int
    return 1;
  end Find;
end K;
