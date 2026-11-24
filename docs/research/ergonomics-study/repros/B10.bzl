// bug: B10
// title: Enum `NameOf` / `FromNameOrNull` are not found
// issue: #3862
// rests-on: stdlib §9.9 / spec §10.4: `NameOf` and `FromNameOrNull` are compiler-provided on every enum
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: Green
// stdout: True
module P;
public enum Color
  Red, Green;
end Color;
public class Program
  public static method Main()
    Console.WriteLine(Color.NameOf(Color.Green));
    Console.WriteLine(Color.FromNameOrNull("Red") <> null);
  end Main;
end Program;
