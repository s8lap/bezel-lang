//! case: s28_15_0014
//! doc: spec
//! section: 28.15
//! title: under the report mode, a holder declared after the sibling field it carries reports nothing
//! rule: Two order tests apply where such a store places an instance in an `owned`
//!   field `f` of `this` and the instance's release may run user code (*The
//!   bound*, above): if the instance carries the value of an `owned` field `g` of
//!   `this`, `g` must be declared before `f` ... The cascade then disposes `f`
//!   first (§28.4). — §28.15 *The callee side*.
//! cites: 28.3, 28.4
//! expect: quiet
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
  private owned _db: Db;
  private owned _cache: Cache;

  public constructor()
    _db := new Db();
    _cache := new Cache(_db);
  end constructor;
end Root;
