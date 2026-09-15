//! case: s9_7_0010
//! doc: spec
//! section: 9.7
//! title: a record of runtime-managed fields is not borrow-bearing under transitivity
//! rule: A record whose only reference-typed fields are of declared
//!   runtime-managed types (`string`, function types) is not a borrow-bearing
//!   record, so a record holding one is not borrow-carrying through the
//!   transitivity clause either, and no borrow-lifetime rule applies.
//! cites: 3.6, 28.9
//! expect: accept
module Conf;

public record Label(Text: string);

public record Session(At: Label, Id: int);

public class Cache
  private _session: Session;

  public constructor(session: Session)
    _session := session;
  end constructor;

  public method Id(): int => _session.Id;
end Cache;
