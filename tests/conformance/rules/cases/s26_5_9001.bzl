//! case: s26_5_9001
//! doc: spec
//! section: 26.5
//! title: an extension property over Stack<int> is what `?.` selects on a nullable Stack<int>
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the current
//!   module and all imported modules
//! cites: 7.5
//! expect: run
//! output: 7
//!   5
//! notes: Harvested from the #8783 review (probes2/ext_prop.bzl), an admit control: the head's documented
//!   surface does not declare `Total` or `Mid`, so rule 1 commits nothing and rule 2 finds the extension
//!   blocks. Trace: `q?.Total` is 7 and `a?.Mid` is 5, both non-null receivers.
module Conf;
extension Stack<int> public property Total: int => 7; end;
extension Array<int> public property Mid: int => 5; end;
public class Program
  public static method Main()
    var q: Stack<int>? := new Stack<int>();
    var r := q?.Total ?? 0;
    var a: Array<int>? := new Array<int>();
    var m := a?.Mid ?? 0;
    Console.WriteLine(r);
    Console.WriteLine(m);
  end Main;
end Program;
