//! case: s8_6_4707
//! doc: spec
//! section: 8.6
//! title: a null Guid? coalesced to Guid.Empty boxes as a Guid, and a null Guid? boxes to a null Object?
//! rule: If the value is `null`, the result is a null `Object` reference.
//! cites: 7.7, 5.1
//! expect: run
//! output: 00000000-0000-0000-0000-000000000000
//!   True
module Conf;

public class Program
  public static method Main()
    var m: Guid? := null;
    var o: Object := m ?? Guid.Empty;
    Console.WriteLine(o.ToString());
    var q: Object? := m;
    Console.WriteLine(q = null);
  end Main;
end Program;
