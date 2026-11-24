// bug: NB16
// title: B1 residual: string members on a local inferred from `Console.In.ReadToEnd()` lower to nothing
// issue: new (closed #8383 covered the direct path, fad5f5e31)
// rests-on: stdlib §7.4 `ReadToEnd(): string`; stdlib §9.6 string members
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: quiet
// stdout: abc
// stdout: HI
// stdout: Hi
// stdout: hi
module P;
public class Program
  public static method Main()
    var a := "AbC";
    var b := a.ToLower();
    Console.WriteLine(b);
    var raw := Console.In.ReadToEnd();
    var c := raw.ToUpper();
    Console.WriteLine(c);
    var d := raw.Trim();
    Console.WriteLine(d);
    var e: string := raw;
    Console.WriteLine(e.ToLower());
  end Main;
end Program;
