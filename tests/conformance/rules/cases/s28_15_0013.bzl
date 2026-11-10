//! case: s28_15_0013
//! doc: spec
//! section: 28.15
//! title: under the report mode, a holder declared before the sibling field it carries reports
//! rule: Two order tests apply where such a store places an instance in an `owned`
//!   field `f` of `this` and the instance's release may run user code (*The
//!   bound*, above): if the instance carries the value of an `owned` field `g` of
//!   `this`, `g` must be declared before `f` ... The cascade then disposes `f`
//!   first (§28.4). — §28.15 *The callee side*. The mode also emits one warning
//!   at each site that is not a store and that §28.15 rejects, naming the rule.
//!   (§28.3.12 clause 4)
//! cites: 28.3, 28.4
//! expect: warn
//! diagnostic: declared before
//! flags: --report-trust-admissions
//! issue: 8423
module Conf;

public class Db
  public constructor()
  end constructor;

  public method N(): int => 1;
end Db;

public class Cache
  private _db: Db? := null;

  public constructor(db: Db)
  end constructor;

  destructor
  end destructor;
end Cache;

public class Root
  private owned _cache: Cache;
  private owned _db: Db;

  public constructor()
    _db := new Db();
    _cache := new Cache(_db);
  end constructor;
end Root;
