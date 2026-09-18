# Native methods are resolved by name at runtime, so nothing that JNI looks up
# may be renamed or stripped.
-keepclasseswithmembernames class * {
    native <methods>;
}
