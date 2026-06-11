public class Order(id: int) serialized tree : ISerializable, IDeserializable
  public owned Customer: Customer;        // nested owned class, also tree
  public owned Lines: OwnedArray<owned LineItem>;
  public readonly property Id: int => id;
  // compiler synthesizes Serialize / Deserialize; each owned child
  // recurses through its own synthesized members
end Order;
