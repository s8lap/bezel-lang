//! case: s9_8_0052
//! doc: spec
//! section: 9.8
//! title: a class with a collection field that declares neither serialization interface is unaffected
//! rule: A class opts into serialization synthesis by declaring `: ISerializable` and/or `:
//!   IDeserializable`
//! cites: 9.8
//! expect: run
//! stdout: 0
//! issue: 8391
module Conf;

public class Person
  public property Name: string := "";
  public owned Tags: Array<string> := new Array<string>();
end Person;

public class Program
  public static method Main()
    using var p := new Person();
    Console.WriteLine(p.Tags.Count);
  end Main;
end Program;
