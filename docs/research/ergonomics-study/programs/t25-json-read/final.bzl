module Json.Read;

import System.Serialization;

// Hand-driving objects of unknown shape is not expressible (IDeserializer has no end-of-object
// probe); declare the element shape and let the synthesized Deserialize skip unknown fields.
public record Item(id: int, price: double): IDeserializable;

public class Program
  public static method Main()
    const text := "[{\"id\":1,\"price\":2.5,\"extra\":true},{\"id\":2,\"price\":4.0}]";
    using var d := new JsonDeserializer(text, new JsonOptions(false, 2, false, true, 64));
    var count := 0;
    var total := 0.0;
    _ := d.BeginArray();
    while d.HasMoreElements() do
      var item := Item.Deserialize(d);
      total += item.price;
      count++;
    end;
    d.EndArray();
    Console.WriteLine($"items {count} total {total}");
  end Main;
end Program;
