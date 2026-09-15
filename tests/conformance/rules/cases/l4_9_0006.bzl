//! case: l4_9_0006
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary<K,V> provides no default entry iteration
//! rule: Default for var entry in ownedDict do iteration is not provided
//!   on OwningDictionary<K, V> — the interface conformance is IDisposable
//!   only, not IOwningCollection<DictionaryEntry<K, V>>.
//! cites: 4.9
//! expect: reject
//! diagnostic: IEnumerable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var d := new OwningDictionary<string, Widget>();
    for var entry in d do
    end;
  end Demo;
end Program;
