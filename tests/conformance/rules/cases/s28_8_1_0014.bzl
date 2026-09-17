//! case: s28_8_1_0014
//! doc: spec
//! section: 28.8.1
//! title: an argument-position move whose restoration follows a throwable call is rejected
//! rule: A method call is throwable, so an argument-position move whose
//!   restoration follows the call is rejected on the call's exception edge.
//! cites: 28.8.1
//! expect: reject
//! diagnostic: not reassigned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Helper
  public static method TakeOwnership(owned resource: Widget)
  end TakeOwnership;
end Helper;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public method TransferToHelper()
    var fresh := new Widget();
    Helper.TakeOwnership(move _resource);
    _resource := fresh;
  end TransferToHelper;
end Holder;
