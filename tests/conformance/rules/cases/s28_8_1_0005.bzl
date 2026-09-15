//! case: s28_8_1_0005
//! doc: spec
//! section: 28.8.1
//! title: a throwable construction between the move and the restoration is an unrestored exception edge
//! rule: A throwable expression between the move and the reassignment
//!   introduces an exit path on which the reassignment has not run — the
//!   exception edge is an unrestored exit.
//! cites: 28.8.1
//! expect: reject
//! diagnostic: not reassigned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class DangerousFactory
  public static method Make(): owned Widget
    throw new InvalidOperationException("boom");
  end Make;
end DangerousFactory;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public method WrongOrder(): owned Widget
    var old := move _resource;
    var fresh := DangerousFactory.Make();
    _resource := fresh;
    return old;
  end WrongOrder;
end Holder;
