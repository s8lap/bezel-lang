record BitMask(Bits: ulong)
  public static method operator ~(m: BitMask): BitMask =>
    new BitMask(~m.Bits);
end BitMask;
