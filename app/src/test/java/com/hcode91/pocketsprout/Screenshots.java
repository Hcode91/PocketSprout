package com.hcode91.pocketsprout;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.view.View;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowDialog;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Renders the tutorial screenshots in docs/screenshots off-device. No firmware runs here, so the
 * picture on the screen is an original placeholder drawing, not anything from a real Didj.
 *
 *   gradle :app:testDebugUnitTest --tests '*Screenshots*'
 */
@RunWith(RobolectricTestRunner.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = 34, qualifiers = "w891dp-h411dp-land-xxhdpi")
public class Screenshots {
    private static final int W = 2340, H = 1080;
    private final File out = new File(System.getProperty("screenshots.dir", "../docs/screenshots"));

    private void save(View v, String name, int w, int h) throws Exception {
        out.mkdirs();
        v.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                h > 0 ? View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY) : View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        if (h <= 0) h = v.getMeasuredHeight();
        v.layout(0, 0, w, h);
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        v.draw(new Canvas(bmp));
        Bitmap small = Bitmap.createScaledBitmap(bmp, w / 2, h / 2, true);
        try (FileOutputStream f = new FileOutputStream(new File(out, name + ".png"))) {
            small.compress(Bitmap.CompressFormat.PNG, 100, f);
        }
    }

    /** An original 320 x 240 scene standing in for the console's picture. */
    private static int[] placeholder() {
        Bitmap b = Bitmap.createBitmap(Native.W, Native.H, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setShader(new LinearGradient(0, 0, 0, 240, Color.rgb(0x7F, 0xD4, 0xF5), Color.rgb(0xE6, 0xF7, 0xD9), Shader.TileMode.CLAMP));
        c.drawRect(0, 0, 320, 240, p);
        p.setShader(null);
        p.setColor(Color.rgb(0x5C, 0xB8, 0x5C));
        Path hill = new Path();
        hill.moveTo(0, 170); hill.quadTo(110, 130, 200, 165); hill.quadTo(270, 190, 320, 160); hill.lineTo(320, 240); hill.lineTo(0, 240); hill.close();
        c.drawPath(hill, p);
        p.setColor(Color.rgb(0x3E, 0x8E, 0x5E));
        c.drawRect(155, 120, 161, 170, p);
        c.drawOval(120, 100, 158, 124, p);
        c.drawOval(158, 92, 198, 118, p);
        p.setColor(Color.rgb(0x1F, 0x3A, 0x2A));
        p.setTextAlign(Paint.Align.CENTER);
        p.setTextSize(26);
        p.setFakeBoldText(true);
        c.drawText("YOUR GAME HERE", 160, 52, p);
        int[] px = new int[Native.W * Native.H];
        b.getPixels(px, 0, Native.W, 0, 0, Native.W, Native.H);
        return px;
    }

    @Test
    public void screens() throws Exception {
        MainActivity act = Robolectric.buildActivity(MainActivity.class).setup().get();
        View content = act.findViewById(android.R.id.content);
        EmuView view = (EmuView) ((android.view.ViewGroup) content).getChildAt(0);

        // 1: first launch, before a NAND dump is chosen.
        save(view, "1-first-launch", W, H);

        // 2: playing.
        view.setMessage(null);
        view.setCoreName("JIT");
        view.pushFrame(placeholder());
        save(view, "2-playing", W, H);

        // 3: the MENU.
        java.lang.reflect.Method m = EmuView.class.getDeclaredMethod("showMenu");
        m.setAccessible(true);
        m.invoke(view);
        AlertDialog d = (AlertDialog) ShadowDialog.getLatestDialog();
        View dv = d.getWindow().getDecorView();
        save(dv, "3-menu", 1100, 1180);
    }
}
