module Json.RoundTrip;

import System.Serialization;

// v2: a class owning Array<string> was rejected as not serialization-eligible (compiler bug B22;
// spec §9.8 admits heap sequence collections in class components). Fall back to a record with an
// inline array, which fixes the tag count at 2.
public record Person(Name: string, Age: int, Tags: [2]string): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var tags: [2]string := ["admin", "dev"];
    var p := new Person("Ann", 34, tags);
    using var s := new JsonSerializer();
    p.Serialize(s);
    var text := s.ToString();
    Console.WriteLine(text);
    using var d := new JsonDeserializer(text);
    var back := Person.Deserialize(d);
    Console.WriteLine($"roundtrip equal: {back = p}");
  end Main;
end Program;
