//! case: s26_5_0027
//! doc: spec
//! section: 26.5
//! title: imported public extensions on Array<int>: Sum is selected, the instance Contains wins by name
//! rule: Extension resolution searches the current module and all imported modules (spec 26.5 rule 2, v0.350); a public extension block imported from another module is ranked by the same specificity ordering (spec 26.5.1), and instance members still win by name (rule 1).
//! cites: 26.5, 26.5.1
//! expect: run
//! output: 77
//!   True
//!   77
module R;
import L1;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Sum()); Console.WriteLine(a.Contains(2));
  var n: Array<int>? := a; Console.WriteLine(n?.Sum() ?? -1);
  for var i in 0..100 do var b: Array<int> := [1,2,3]; if b.Sum() <> 77 then return 1; end; end;
  return 0;
end Main; end Program;
