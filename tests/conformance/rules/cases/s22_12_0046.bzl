//! case: s22_12_0046
//! doc: spec
//! section: 22.12
//! title: dropping readonly from the same primitive static draws form (iii)'s warning
//! rule: **(iii) Warned value static:** a mutable static whose declared type is a
//!   primitive value type ... the obligation it names is to confine every read and
//!   write of the field to one domain, or to move to a synchronized spelling.
//! cites: 22.12, 30.14
//! expect: warn
//! diagnostic: one domain
module Conf;

public static class Policy
  private static _retries: int := 3;

  public static method Budget(): int
    return Policy._retries;
  end Budget;
end Policy;
