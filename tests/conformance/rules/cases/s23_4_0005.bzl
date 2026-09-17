//! case: s23_4_0005
//! doc: spec
//! section: 23.4
//! title: indexers overload by parameter type list
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 22.6
//! expect: accept
module Conf;

public class Table
  private owned _byIndex: Array<string> := new Array<string>();
  private owned _byKey: Dictionary<string, string> := new Dictionary<string, string>();

  public constructor()
  end constructor;

  public property [i: int]: string => _byIndex[i];
  public property [key: string]: string => _byKey[key];
end Table;
