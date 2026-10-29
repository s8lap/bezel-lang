//! case: s26_5_0028
//! doc: spec
//! section: 26.5
//! title: imported exts on LinkedList<int> and Set<int>: Sum and Any selected, instance Last and Contains win
//! rule: Extension resolution searches the current module and all imported modules (spec 26.5 rule 2, v0.350); a public extension block imported from another module is ranked by the same specificity ordering (spec 26.5.1), and instance members still win by name (rule 1).
//! cites: 26.5, 26.5.1
//! expect: run
//! output: 77
//!   6
//!   78
//!   True
module R;
import L3;
class Program public static method Main(): int
  var l := new LinkedList<int>(); l.AddLast(5); l.AddLast(6);
  Console.WriteLine(l.Sum());
  Console.WriteLine(l.Last ?? -1);
  var s := new Set<int>(); s.Add(1);
  Console.WriteLine(s.Any()); Console.WriteLine(s.Contains(1));
  return 0;
end Main; end Program;
