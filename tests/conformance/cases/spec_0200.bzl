// ERROR: field _resource was moved at line 2 and not reassigned
//        on the path through the false branch of `if replace` (line 3).
public method Wrong(replace: bool): owned Stream
  var old := move _resource;
  if replace then
    _resource := new Stream();
  end;
  return old;
end Wrong;
