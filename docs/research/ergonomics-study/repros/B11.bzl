// bug: B11
// title: Checked-disposal mode raises `DisposedAccessException` on live sets
// issue: #6314
// rests-on: spec §28.2: checked mode faults only on access to a logically-disposed instance
// source: study program programs/t15-sets/v1.bzl, verbatim below this header
// note: No minimal repro exists: this is the study program the entry names, copied verbatim.
// expect: run
// stdout: union: api,auth,cache,db,queue,web
// stdout: both: api,db
// stdout: onlyA: cache,web
// stdout: subset: True
module Query.Sets;

public class Program
  private static method Show(label: string, items: IEnumerable<string>)
    var sorted := items.ToArray();
    sorted.Sort();
    Console.WriteLine($"{label}: {String.Join(",", sorted)}");
  end Show;

  public static method Main()
    var a: Set<string> := ["web", "api", "db", "cache"];
    var b: Set<string> := ["db", "queue", "api", "auth"];
    Show("union", a.Union(b));
    Show("both", a.Intersect(b));
    Show("onlyA", a.Except(b));
    var probe: Set<string> := ["api", "db"];
    Console.WriteLine($"subset: {probe.IsSubsetOf(a)}");
  end Main;
end Program;
