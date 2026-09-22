//! case: s29_12_0010
//! doc: spec
//! section: 29.12
//! title: a dictionary indexer-set on an existing key does not invalidate the enumerator
//! rule: An element-level write that changes no structure does not invalidate:
//!   `dict[existingKey] := v` updating a present key leaves the enumerator
//!   valid. (The runtime's dict entry-for loop reads a key/value snapshot
//!   taken at loop entry, so a same-pass update is exercised here for its
//!   non-invalidation only, not for observing the new value on a later visit
//!   — that half of the sentence is demonstrated by the array arm, s29_12_0002.)
//! cites: 29.12
//! expect: run
//! stdout: 55
module Conf;

public class Program
  public static method Main()
    var ages: Dictionary<string, int> := ["alice": 30, "bob": 25];
    var total := 0;
    for var entry in ages do
      ages[entry.Key] := entry.Value + 1;
      total := total + entry.Value;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
