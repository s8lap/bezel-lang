public method TakeOwnership(owned resource: Stream)
  _resource := resource;     // moves into owned field; previous _resource
                             // must have been disposed first per §28.7
end TakeOwnership;

public static method NewBuffer(size: int): owned Array<byte>
  return new Array<byte>(size);   // owned by virtue of new, returned as owned
end NewBuffer;
