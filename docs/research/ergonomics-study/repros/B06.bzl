// bug: B6
// title: A string read through an indexer or field and passed straight into a record constructor is freed with its source
// issue: #8386
// rests-on: spec §28.1/§28.9: `string` is runtime-managed; a record copies its components
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: name1
// stdout: name2
// stdout: name3
module P;
record E(Name: string);
public class Program
  public static method Main()
    var rows := new Array<E>();
    for var i in 1..3 do
      var f := $"name{i},z".Split(',');
      rows.Add(new E(f[0]));
    end;
    var junk := new Array<string>();
    for var i in 1..100 do junk.Add($"junkjunkjunk{i}"); end;
    for var r in rows do Console.WriteLine(r.Name); end;
  end Main;
end Program;
