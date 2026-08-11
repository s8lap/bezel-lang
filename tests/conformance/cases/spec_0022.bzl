record Rectangle
  public readonly property Width: decimal;
  public readonly property Height: decimal;

  public constructor(width: decimal, height: decimal)
    Width := width;
    Height := height;
  end constructor;

  public method Area(): decimal => Width * Height;
end Rectangle;
