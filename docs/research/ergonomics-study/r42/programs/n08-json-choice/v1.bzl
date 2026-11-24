module Draw.Json;

import System.Serialization;

public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double);

  public method Describe(): string => (case this of
    Circle(var r) => $"Circle {r}",
    Rect(var w, var h) => $"Rect {w}x{h}");
end Shape;

public class Drawing: ISerializable, IDeserializable
  public readonly property Title: string;
  public owned readonly Shapes: Array<Shape>;

  public constructor(title: string, owned shapes: Array<Shape>)
    Title := title;
    Shapes := shapes;
  end constructor;
end Drawing;

public class Program
  public static method Main()
    var shapes: Array<Shape> := [Shape.Circle(1.5), Shape.Rect(2.0, 3.0), Shape.Circle(0.5)];
    using var drawing := new Drawing("demo", move shapes);
    using var s := new JsonSerializer();
    drawing.Serialize(s);
    var text := s.ToString();
    Console.WriteLine(text);
    using var d := new JsonDeserializer(text);
    using var back := Drawing.Deserialize(d);
    for var sh in back.Shapes do
      Console.WriteLine(sh.Describe());
    end;
    Console.WriteLine($"count {back.Shapes.Count}");
  end Main;
end Program;
