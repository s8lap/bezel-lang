// bug: B16
// title: A collection literal at an `IEnumerable<string>` parameter is accepted silently and does nothing
// issue: #8390
// rests-on: spec §15.4 names no interface-typed literal target, so the call is ill-formed and must be rejected
// source: data/compiler-bugs.md, verbatim below this header
// expect: reject
// diagnostic: §15.4
module P;
import System.IO;
public class Program
  public static method Main()
    const p := "/var/tmp/bezel-scratch/ergo/f1.txt";
    File.WriteAllLines(p, ["alpha", "beta"]);
    Console.WriteLine(File.Exists(p));
    var t := File.ReadAllText(p);
    Console.WriteLine(t.Length);
    File.WriteAllText(p, "x\ny\n");
    Console.WriteLine(File.ReadAllText(p).Length);
    for var l in File.ReadAllLines(p) do Console.WriteLine(l); end;
    var xs: Array<string> := ["q", "r"];
    File.WriteAllLines(p, xs);
    Console.WriteLine(File.ReadAllText(p).Length);
  end Main;
end Program;
