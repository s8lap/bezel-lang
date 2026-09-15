//! case: s29_2_0002
//! doc: spec
//! section: 29.2
//! title: the for variable over string-typed elements is a shared reference
//! rule: The `for` desugaring binds `var x := __enumerator.Current;`, and for a
//!   shared-reference slot — a runtime-managed element type such as `string` —
//!   the element operation *indexer-get / default iteration* is "reference
//!   out" (§28.11's element slot policy); no ownership transfers.
//! cites: 29.2, 28.11, 28.9
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var names: Array<string> := ["ab", "xyz"];
    var total := 0;
    for var s in names do
      total := total + s.Length;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
