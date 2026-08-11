private owned _resource: Stream?;        // nullable

public method Detach(): owned Stream?
  if _resource = null then return null; end;
  var taken := move _resource!;          // move out the non-null contents
  _resource := null;                      // restore (to null)
  return taken;
end Detach;
