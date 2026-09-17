//! case: s22_2_0004
//! doc: spec
//! section: 22.2
//! title: an expression body under a declared choice return type target-types a bare case
//! rule: An expression body under a declared return type is a
//!   declared-result position: a bare choice-case construction in it
//!   target-types against the declared type, exactly as a `return` operand
//!   does.
//! cites: 22.2, 35.3
//! expect: accept
module Conf;

public choice FetchOutcome
  Fetched(status: int),
  NotModified;
end FetchOutcome;

public class Fetcher
  public constructor()
  end constructor;

  public method CachedOutcome(): FetchOutcome => NotModified;
end Fetcher;
