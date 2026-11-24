// bug: B14
// title: `File.Delete` draws the unused-result warning
// issue: #8389
// rests-on: stdlib §7.3 `Delete(path: string)` returns `()`; spec §21.9 warns only on a non-`()` result
// source: data/compiler-bugs.md, verbatim below this header
// note: The runner points /var/tmp/bezel-scratch/ at a fresh scratch directory with ergo/bugs created.
// expect: quiet
module P;
import System.IO;
public class Program
  public static method Main()
    File.WriteAllText("/var/tmp/bezel-scratch/ergo/bugs/x.txt", "x");
    File.Delete("/var/tmp/bezel-scratch/ergo/bugs/x.txt");
  end Main;
end Program;
