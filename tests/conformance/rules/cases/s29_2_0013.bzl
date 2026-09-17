//! case: s29_2_0013
//! doc: spec
//! section: 29.2
//! title: iterating an OwningDictionary's Keys view is the borrowing remedy
//! rule: Where `OwningDictionary<K, V>` provides no default entry iteration,
//!   the borrowing remedy is the key-iteration pattern: `Keys` is an
//!   `IReadOnlyCollection<K>` view, which implements `IEnumerable<K>` and is
//!   therefore a legal `for` source (stdlib §4.9).
//! cites: 29.1, 29.2
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Widget>();
    od["a"] := new Widget();
    var count := 0;
    for var key in od.Keys do
      count := count + 1;
    end;
    Console.WriteLine(count);
  end Main;
end Program;
