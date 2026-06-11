public record Point(X: int, Y: int): ISerializable, IDeserializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): Point
    d.BeginObject();
    var x := 0;  var y := 0;
    // field-name dispatch omitted for brevity; a real implementation
    // loops over ReadFieldName() and fills the locals
    d.EndObject();
    return new Point(x, y);
  end Deserialize;
end Point;
