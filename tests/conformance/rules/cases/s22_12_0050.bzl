//! case: s22_12_0050
//! doc: spec
//! section: 22.12
//! title: a static owned readonly OwningTask field is a compile error at the declaration
//! rule: A `static readonly` field or auto-property of type `OwningTask<T>` or
//!   `OwningTask<T>?` — `owned` or not — is a **compile error at the
//!   declaration site** naming this rule, as its mutable form already is.
//! cites: 22.12, 30.4, 30.5
//! expect: reject
//! diagnostic: OwningTask
//! issue: 5899
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Cache
  private static owned readonly _t: OwningTask<Res> := Make();

  private static async method Make(): OwningTask<Res>
    await Task.Delay(1);
    return new Res();
  end Make;
end Cache;
