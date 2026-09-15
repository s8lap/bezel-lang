//! case: s9_2_0020
//! doc: spec
//! section: 9.2
//! title: equality skip on a static field is an inert-annotation compile error
//! rule: The equality skip clause is admitted on storage components only; on a
//!   static member it is a compile error, mirroring section 9.8's
//!   inert-annotation rule, because a static member never participates in
//!   equality.
//! cites: 9.8, A.4
//! expect: reject
//! diagnostic: equality skip
module Conf;

public record Config
  public static readonly equality skip Default: int := 0;
  public readonly property Value: int;

  public constructor(value: int)
    Value := value;
  end constructor;
end Config;
