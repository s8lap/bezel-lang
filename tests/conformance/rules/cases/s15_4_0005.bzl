//! case: s15_4_0005
//! doc: spec
//! section: 15.4
//! title: a fresh collection literal returned through a non-owned instance return is rejected
//! rule: A fresh literal returned through a non-`owned` instance return is
//!   rejected exactly as any freshly-produced owned value is (§15.4, §28.6).
//! cites: 15.4, 28.6
//! expect: reject
//! diagnostic: owned
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Factory
  public constructor()
  end constructor;

  public method Build(): OwningArray<Connection>
    return [new Connection(), new Connection()];
  end Build;
end Factory;
