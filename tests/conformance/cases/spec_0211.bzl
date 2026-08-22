public method Resize(newSize: int)
  _buffer := new Array<byte>(newSize, 0);   // sized ctor (stdlib §4.4); previous _buffer disposed here, implicitly
end Resize;
