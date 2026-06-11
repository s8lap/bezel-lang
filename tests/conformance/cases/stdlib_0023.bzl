public class Node(label: string) serialized cyclic : ISerializable, IDeserializable
  public owned Next: Node;          // owned forward edge
  public Parent: Node;              // back-edge: may close a cycle
  public readonly property Label: string => label;
  // constructor is self-contained: assigns only its own fields,
  // reads no back-edge, never lets `this` escape — so it is
  // cyclic-serialization-eligible
end Node;
