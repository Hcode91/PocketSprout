package com.hcode91.didjdroid;

/** JNI entry points into the C++ emulator core. */
public final class Native {
    static { System.loadLibrary("didjdroid"); }

    public static final int W = 320, H = 240;

    // Button bits (must match core/src/didj.h)
    public static final int LEFT = 1, RIGHT = 2, UP = 4, DOWN = 8, L = 16, R = 32, A = 64, B = 128,
            HOME = 256, HINT = 512, PAUSE = 1024, BRIGHTNESS = 2048;

    /** Returns an empty string on success, otherwise an error message. */
    public static native String init(String nandPath);
    public static native void runFrame(int[] pixels);
    public static native void setButtons(int mask);
    public static native int readAudio(short[] buf);
    public static native boolean saveNand(String path);
    public static native void reset();
    /** Loads a cartridge (FAT image of the cart volume, or raw NAND dump). Empty string = OK. */
    public static native String loadCart(String path);
    public static native void ejectCart();
    /** Switches the ARM64 recompiler on/off; returns whether it is active. */
    public static native boolean setJit(boolean on);
    public static native boolean jitAvailable();
    public static native String console();
}
