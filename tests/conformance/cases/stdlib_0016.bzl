public static method Deserialize(d: IDeserializer): T;          // value-type
                                                                //   target (record,
                                                                //   enum): no `owned`
public static method Deserialize(d: IDeserializer): owned T;     // class target:
                                                                //   `owned` return
// T = the declaring type, written concretely.
