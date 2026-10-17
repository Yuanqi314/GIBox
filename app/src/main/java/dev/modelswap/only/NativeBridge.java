package dev.modelswap.only;

/** All native entry points are implemented in app/src/main/cpp. */
public final class NativeBridge {
    private static boolean loaded;
    private NativeBridge() {}
    public static synchronized void loadModule(String absoluteLibrary) {
        if (!loaded) { System.load(absoluteLibrary); loaded=true; }
    }
    public static synchronized byte[] decodeTexture(byte[] dds) {
        if (!loaded) { System.loadLibrary("gibox"); loaded=true; }
        return decodeDds(dds);
    }
    public static native void start(String packageName,String privateDir);
    private static native byte[] decodeDds(byte[] dds);
}
