//! case: s30_14_0009
//! doc: spec
//! section: 30.14
//! title: the nullable form of a sendable record satisfies where T sendable
//! rule: A **nullable type** `V?` is sendable exactly where `V` is (§3.6
//!   *Nullable forms*): `int?` and `Job?` for a sendable `Job` cross as `V`
//!   does, the absent value included
//! cites: 30.14, 3.6, 14.2
//! expect: accept
module Conf;

public record Job(Id: int);

public class Ship
  public static method Count<T>(x: T): int where T: sendable => 1;

  public static method Run(): int
    var j: Job? := new Job(4);
    return Ship.Count<Job?>(j) + Ship.Count<int?>(null);
  end Run;
end Ship;
