//! case: s1_6_0001
//! doc: spec
//! section: 1.6
//! title: a method's end label matching its name is accepted
//! rule: Named constructs — method bodies included — take the identifier as
//!   the optional end label: `end methodName;`.
//! expect: accept
module Conf;

public class Holder
  private method Compute(): int
    return 1;
  end Compute;
end Holder;
