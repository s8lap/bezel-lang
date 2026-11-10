module Json.Read;

import System.Serialization;

public class Program
  public static method Main()
    const text := "[{\"id\":1,\"price\":2.5,\"extra\":true},{\"id\":2,\"price\":4.0}]";
    // strict by default: SkipValue on an unknown field raises; opt in to tolerance (all five options required).
    using var d := new JsonDeserializer(text, new JsonOptions(false, 2, false, true, 64));
    var count := 0;
    var total := 0.0;
    _ := d.BeginArray();
    while d.HasMoreElements() do
      _ := d.BeginObject();
      while d.HasMoreElements() do
        var name := d.ReadFieldName();
        case name of
          "id" => _ := d.ReadInt(),
          "price" => total += d.ReadFloat(),
          _ => d.SkipValue()
        end;
      end;
      d.EndObject();
      count++;
    end;
    d.EndArray();
    Console.WriteLine($"items {count} total {total}");
  end Main;
end Program;
