module Draw.Json;

import System.Serialization;

// r42: an array of choices serializes, but its synthesized Deserialize fails at the second
// element ("Choice deserialize: expected '{'"; new). Compromise: a record with a kind tag.
public enum ShapeKind
  Circle, Rect;
end ShapeKind;

public record Shape(Kind: ShapeKind, A: double, B: double): ISerializable, IDeserializable
  public method Describe(): string => (case Kind of
    ShapeKind.Circle => $"Circle {A}",
    ShapeKind.Rect => $"Rect {A}x{B}");
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
    // r42: a collection-literal local of record elements is Borrowed, so `move` is rejected (#8498)
    var shapes := new Array<Shape>();
    shapes.Add(new Shape(ShapeKind.Circle, 1.5, 0.0));
    shapes.Add(new Shape(ShapeKind.Rect, 2.0, 3.0));
    shapes.Add(new Shape(ShapeKind.Circle, 0.5, 0.0));
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
