public record Point(X: int, Y: int): ISerializable
  public method Serialize(s: ISerializer)
    s.BeginObject(2);
    s.WriteFieldName("X"); s.WriteInt(X);
    s.WriteFieldName("Y"); s.WriteInt(Y);
    s.EndObject();
  end Serialize;
end Point;
