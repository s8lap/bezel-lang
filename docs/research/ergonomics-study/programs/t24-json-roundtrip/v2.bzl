module Json.RoundTrip;

import System.Serialization;

// A record may not hold a heap collection (spec §9.8 record-admissible), so Person is a class
// that owns its Tags. The Json facade is not implemented (#2349): drive the agents by hand.
public class Person: ISerializable, IDeserializable
  public property Name: string;
  public property Age: int;
  public owned Tags: Array<string> := new Array<string>();
end Person;

public class Program
  public static method Main()
    using var p := new Person();
    p.Name := "Ann";
    p.Age := 34;
    p.Tags.Add("admin");
    p.Tags.Add("dev");
    using var s := new JsonSerializer();
    p.Serialize(s);
    var text := s.ToString();
    Console.WriteLine(text);
    using var d := new JsonDeserializer(text);
    using var back := Person.Deserialize(d);
    var same := back.Name = p.Name and back.Age = p.Age and back.Tags.Count = p.Tags.Count;
    Console.WriteLine($"roundtrip equal: {same}");
  end Main;
end Program;
