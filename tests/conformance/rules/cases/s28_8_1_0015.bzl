//! case: s28_8_1_0015
//! doc: spec
//! section: 28.8.1
//! title: restoring the field before the call that may throw fixes the argument-position hazard
//! rule: The fix is to construct the replacement and reassign before the
//!   call — the same "build then take then restore" discipline, with the
//!   call moved to after the restore.
//! cites: 28.8.1
//! expect: accept
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

  public method TransferToHelperSafe()
    var fresh := new Widget();
    var taken := move _resource;
    _resource := fresh;
    Helper.TakeOwnership(move taken);
  end TransferToHelperSafe;
end Holder;
