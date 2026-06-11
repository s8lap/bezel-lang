public class Invoice(number: string) serialized graph : ISerializable, IDeserializable
  public owned Seller: Company;           // owned: in the owning spanning tree
  public owned Buyer: Company;            // owned
  public AccountManager: Employee;        // borrow: a back-edge, e.g. an
                                          //   Employee owned by Seller
  public readonly property Number: string => number;
end Invoice;
