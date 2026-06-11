public class FrameBuffer
  private owned _pixels: OwnedBuffer<Pixel>;
  private readonly _width: int;

  // Width and Height as needed; Pixel is assumed to be a class type.

  public method ScanLineReadOnly(y: int): ReadOnlySpan<Pixel> =>
    new ReadOnlySpan<Pixel>(_pixels, y * _width, _width);

  public method ScanLine(y: int): Span<Pixel> =>
    new Span<Pixel>(_pixels, y * _width, _width);
end FrameBuffer;
