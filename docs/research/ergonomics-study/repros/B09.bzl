// bug: B9
// title: `ParseOrNull` on the integer types is not found
// issue: #7318
// rests-on: stdlib §9.1: `static pure ParseOrNull(s: string): T?` on every integer type
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 12 12 12
module P;
public class Program
  public static method Main()
    var a := int.ParseOrNull("12");
    var b := Int32.ParseOrNull("12");
    var c := long.ParseOrNull("12");
    Console.WriteLine($"{a} {b} {c}");
  end Main;
end Program;
