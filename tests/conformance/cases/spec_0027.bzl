public abstract class Shape: ISerializable, IDeserializable
  polymorphic ("circle": Circle, "square": Square);
  // ... other members ...
end Shape;
