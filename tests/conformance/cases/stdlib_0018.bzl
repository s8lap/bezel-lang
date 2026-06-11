public record Point(X: int, Y: int): ISerializable, IDeserializable;
// compiler synthesizes Serialize(s: ISerializer) and the recognized
// static Deserialize(d: IDeserializer): Point — no body needed
