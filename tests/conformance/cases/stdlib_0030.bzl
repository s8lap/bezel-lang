public class SerializationException: Exception
  public readonly property Offset: long?;   // byte offset of the fault in the input (binary; null for JSON)
  public readonly property Line: int?;      // line of the fault in the input text (JSON; null for binary)
  public readonly property Column: int?;    // column of the fault in the input text (JSON; null for binary)
end SerializationException;
