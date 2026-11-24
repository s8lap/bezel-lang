// bug: B19
// title: A class owning an `Array<string>` is not serialization-eligible
// issue: #8391
// rests-on: spec §9.8 (v0.355): a standard sequence collection whose element type is admissible is admissible in class components
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 0
module P;
public class Person: ISerializable
  public property Name: string := "";
  public owned Tags: Array<string> := new Array<string>();
end Person;
public class Program
  public static method Main()
    using var p := new Person();
    Console.WriteLine(p.Tags.Count);
  end Main;
end Program;
