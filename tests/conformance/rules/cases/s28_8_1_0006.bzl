//! case: s28_8_1_0006
//! doc: spec
//! section: 28.8.1
//! title: building the replacement before the move leaves no throwable edge between move and restoration
//! rule: Build before take — perform any throwable construction before the
//!   move, leaving only non-throwable assignments and the return between
//!   the move and the exits.
//! cites: 28.8.1
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class DangerousFactory
  public static method Make(): owned Widget
    return new Widget();
  end Make;
end DangerousFactory;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public method RightOrder(): owned Widget
    var fresh := DangerousFactory.Make();
    var old := move _resource;
    _resource := fresh;
    return old;
  end RightOrder;
end Holder;
