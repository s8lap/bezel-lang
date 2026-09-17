//! case: s28_11_0005
//! doc: spec
//! section: 28.11
//! title: an OwningDictionary owns its values while its keys are classified as ordinary borrows
//! rule: For dictionaries the classification applies independently to the
//!   key slots and the value slots — an owning dictionary owns its values;
//!   its keys are value-like and classified per the borrowing column.
//! cites: 28.11
//! expect: accept
module Conf;

public class Key
  public constructor()
  end constructor;
end Key;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private owned _byKey: OwningDictionary<Key, Widget>;

  public constructor()
    _byKey := new OwningDictionary<Key, Widget>();
  end constructor;

  public method Register(k: Key)
    _byKey[k] := new Widget();
  end Register;
end Registry;
