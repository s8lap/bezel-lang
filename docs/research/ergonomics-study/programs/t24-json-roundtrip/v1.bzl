module Json.RoundTrip;

import System.Serialization;

public record Person(Name: string, Age: int, Tags: Array<string>): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var tags: Array<string> := ["admin", "dev"];
    var p := new Person("Ann", 34, tags);
    var text := Json.ToText(p);
    Console.WriteLine(text);
    var back := Json.Parse<Person>(text);
    Console.WriteLine($"roundtrip equal: {back.Name = p.Name and back.Age = p.Age and back.Tags.Count = p.Tags.Count}");
  end Main;
end Program;
