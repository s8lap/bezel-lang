/// Compiler-intrinsic; pointer-sized integer (32 or 64 bits depending on target).
public record IntPtr
  public static const Zero: IntPtr;

  public static method operator =(a: IntPtr, b: IntPtr): bool;
  public static method operator <>(a: IntPtr, b: IntPtr): bool;
end IntPtr;

/// Compiler-intrinsic; unsigned pointer-sized integer.
public record UIntPtr
  public static const Zero: UIntPtr;

  public static method operator =(a: UIntPtr, b: UIntPtr): bool;
  public static method operator <>(a: UIntPtr, b: UIntPtr): bool;
end UIntPtr;
