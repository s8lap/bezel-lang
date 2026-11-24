// bug: NB15
// title: `RemoveAt` with a narrowed `int?` emits malformed IR
// issue: #2625 family
// rests-on: spec §7.4: after `if i = null then return; end;` the local is `int`
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 20
// stdout: 2
module P;
public class Program
  public static method Main()
    var xs: Array<int> := [10, 20, 30];
    var i := int.ParseOrNull("1");
    if i = null then return; end;
    Console.WriteLine(xs[i]);
    xs.RemoveAt(i);
    Console.WriteLine(xs.Count);
  end Main;
end Program;
