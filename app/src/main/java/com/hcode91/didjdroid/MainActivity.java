package com.hcode91.didjdroid;

import android.app.Activity;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.net.Uri;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends Activity {
    private static final int PICK_NAND = 1;

    private EmuView view;
    private Thread emuThread;
    private volatile boolean running;
    private volatile boolean started;
    private AudioTrack audio;
    private File nandFile;
    private int keyMask;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        view = new EmuView(this);
        setContentView(view);
        hideSystemUi();
        nandFile = new File(getFilesDir(), "nand.bin");
        if (nandFile.exists()) startEmu();
        else view.setMessage("Tap here to choose your Didj NAND dump\n(didj_29f2g08aac_2cda.bin)");
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    /** Called by EmuView when the user taps the screen area before a NAND is loaded, or the menu. */
    void pickNand() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_NAND);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != PICK_NAND || res != RESULT_OK || data == null) return;
        final Uri uri = data.getData();
        stopEmu();
        view.setMessage("Copying NAND dump...");
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(nandFile)) {
                byte[] buf = new byte[1 << 20];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            } catch (Exception e) {
                runOnUiThread(() -> view.setMessage("Copy failed: " + e.getMessage()));
                return;
            }
            runOnUiThread(this::startEmu);
        }).start();
    }

    private void startEmu() {
        String err = Native.init(nandFile.getAbsolutePath());
        if (!err.isEmpty()) {
            view.setMessage("NAND load failed: " + err + "\nTap to choose another file");
            nandFile.delete();
            return;
        }
        view.setMessage(null);
        started = true;
        resumeEmu();
    }

    private void resumeEmu() {
        if (!started || running) return;
        running = true;
        int minBuf = AudioTrack.getMinBufferSize(32000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        audio = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(32000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(Math.max(minBuf, 32000))
                .setTransferMode(AudioTrack.MODE_STREAM).build();
        audio.play();
        emuThread = new Thread(this::emuLoop, "emu");
        emuThread.start();
    }

    private void emuLoop() {
        int[] px = new int[Native.W * Native.H];
        short[] snd = new short[4096];
        long next = System.nanoTime();
        long fpsStart = System.nanoTime();
        int frames = 0;
        while (running) {
            Native.setButtons(view.getButtons() | keyMask);
            Native.runFrame(px);
            view.pushFrame(px);
            int n;
            while ((n = Native.readAudio(snd)) > 0)
                audio.write(snd, 0, n * 2, AudioTrack.WRITE_NON_BLOCKING);
            frames++;
            long now = System.nanoTime();
            if (now - fpsStart >= 1_000_000_000L) {
                view.setFps(frames * 1e9f / (now - fpsStart));
                frames = 0;
                fpsStart = now;
            }
            next += 16_666_667L;
            long sleep = next - System.nanoTime();
            if (sleep > 0) {
                try { Thread.sleep(sleep / 1_000_000L, (int) (sleep % 1_000_000L)); } catch (InterruptedException ignored) { }
            } else if (sleep < -100_000_000L) {
                next = System.nanoTime(); // running slow: don't try to catch up
            }
        }
    }

    private void stopEmu() {
        running = false;
        if (emuThread != null) {
            try { emuThread.join(); } catch (InterruptedException ignored) { }
            emuThread = null;
        }
        if (audio != null) { audio.release(); audio = null; }
    }

    void resetEmu() {
        if (!started) return;
        stopEmu();
        Native.reset();
        resumeEmu();
    }

    void showConsole() {
        view.toggleConsole(Native.console());
    }

    @Override
    protected void onPause() {
        super.onPause();
        boolean wasStarted = started;
        stopEmu();
        if (wasStarted && Native.saveNand(nandFile.getAbsolutePath()))
            Toast.makeText(this, "Saved", Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
        resumeEmu();
    }

    // ---- physical keys / gamepads ----
    private int keyToButton(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_LEFT: return Native.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return Native.RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP: return Native.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return Native.DOWN;
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_Z: case KeyEvent.KEYCODE_ENTER: return Native.A;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_X: return Native.B;
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_Q: return Native.L;
            case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_W: return Native.R;
            case KeyEvent.KEYCODE_BUTTON_START: return Native.PAUSE;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return Native.HOME;
            case KeyEvent.KEYCODE_BUTTON_Y: return Native.HINT;
        }
        return 0;
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        int b = keyToButton(code);
        if (b != 0) { keyMask |= b; return true; }
        return super.onKeyDown(code, e);
    }

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        int b = keyToButton(code);
        if (b != 0) { keyMask &= ~b; return true; }
        return super.onKeyUp(code, e);
    }
}
