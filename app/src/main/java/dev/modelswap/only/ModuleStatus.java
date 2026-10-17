package dev.modelswap.only;

/**
 * Whether LSPosed loaded GIBox into its own app process: Entry sets these fields when the module's scope includes GIBox
 * itself (handleLoadPackage for dev.modelswap.only). Without that they keep their defaults, so the app reads "not active".
 */
public final class ModuleStatus {
    private ModuleStatus() {}
    public static volatile boolean active;
    /** XposedBridge.getXposedVersion() of the framework that loaded the module. */
    public static volatile int api;
}
