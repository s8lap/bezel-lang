public abstract class Shape serialized graph
    polymorphic ("circle": Circle, "square": Square)
    : ISerializable, IDeserializable
end Shape;

public class Circle(radius: double) serialized tree : Shape
  public readonly property Radius: double => radius;
end Circle;
