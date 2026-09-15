//! case: s35_2_0007
//! doc: spec
//! section: 35.2
//! title: a static owned readonly field is admitted in a choice body
//! rule: A choice body may declare static readonly fields, including static owned
//!   readonly.
//! cites: 35.2, 10.4, 22.12
//! expect: accept
module Conf;

public class Cache
  public constructor()
  end constructor;
end Cache;

public choice Priority
  Low,
  High;

  // Admitted by §35.2. §22.12 rule 4 adds its own declaration-site warning on a
  // static readonly field of class type; that warning is not what this case pins.
  public static owned readonly Shared: Cache := new Cache();

  public method IsUrgent(): bool => (case this of High => true, Low => false);
end Priority;
