abstract class Shape : ISerializable, IDeserializable
  polymorphic("circle": Circle, "square": Square);
end Shape;
