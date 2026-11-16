package com.hcode91.pocketsprout;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** Draws the Didj screen plus on-screen controls, and turns touches into button bits. */
public class EmuView extends View {
    private final MainActivity act;
    private final Bitmap bmp = Bitmap.createBitmap(Native.W, Native.H, Bitmap.Config.ARGB_8888);
    private final int[] frame = new int[Native.W * Native.H];
    private volatile boolean frameDirty;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bmpPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private String message;
    private String console;
    private float fps;
    private volatile int buttons;

    // Layout
    private final Rect screen = new Rect();
    private float dpadX, dpadY, dpadR;
    private final Btn[] btns;
    private final RectF menuRect = new RectF();

    private static class Btn {
        final String label; final int bit; final RectF r = new RectF(); final boolean round;
        Btn(String l, int b, boolean round) { label = l; bit = b; this.round = round; }
    }

    public EmuView(MainActivity a) {
        super(a);
        act = a;
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        btns = new Btn[] {
                new Btn("A", Native.A, true), new Btn("B", Native.B, true),
                new Btn("L", Native.L, false), new Btn("R", Native.R, false),
                new Btn("HOME", Native.HOME, false), new Btn("HINT", Native.HINT, false),
                new Btn("PAUSE", Native.PAUSE, false),
        };
    }

    void setMessage(String m) { message = m; postInvalidate(); }
    void setFps(float f) { fps = f; }
    private String coreName = "";
    void setCoreName(String n) { coreName = n; postInvalidate(); }
    int getButtons() { return buttons; }

    void pushFrame(int[] px) {
        synchronized (frame) { System.arraycopy(px, 0, frame, 0, frame.length); frameDirty = true; }
        postInvalidate();
    }

    void toggleConsole(String text) {
        console = (console == null) ? text : null;
        postInvalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        int sh = (int) (h * 0.92f);
        int sw = sh * 4 / 3;
        if (sw > w * 0.62f) { sw = (int) (w * 0.62f); sh = sw * 3 / 4; }
        int sx = (w - sw) / 2, sy = (h - sh) / 2;
        screen.set(sx, sy, sx + sw, sy + sh);
        float side = sx;
        dpadR = Math.min(side * 0.42f, h * 0.22f);
        dpadX = side / 2;
        dpadY = h * 0.62f;
        float bx = w - side / 2, by = h * 0.62f, br = Math.min(side * 0.18f, h * 0.10f);
        btns[0].r.set(bx + br * 0.3f, by - br * 2.0f, bx + br * 2.3f, by);           // A
        btns[1].r.set(bx - br * 2.3f, by - br * 0.6f, bx - br * 0.3f, by + br * 1.4f); // B
        float lw = side * 0.7f, lh = h * 0.09f;
        btns[2].r.set(side / 2 - lw / 2, h * 0.08f, side / 2 + lw / 2, h * 0.08f + lh);           // L
        btns[3].r.set(w - side / 2 - lw / 2, h * 0.08f, w - side / 2 + lw / 2, h * 0.08f + lh);   // R
        float sbw = side * 0.42f, sbh = h * 0.08f, y0 = h - sbh - h * 0.04f;
        btns[4].r.set(side / 2 - sbw - 4, y0, side / 2 - 4, y0 + sbh);              // HOME
        btns[5].r.set(side / 2 + 4, y0, side / 2 + sbw + 4, y0 + sbh);              // HINT
        btns[6].r.set(w - side / 2 - sbw / 2, y0, w - side / 2 + sbw / 2, y0 + sbh); // PAUSE
        menuRect.set(w - side / 2 - sbw / 2, h * 0.24f, w - side / 2 + sbw / 2, h * 0.24f + sbh);
        text.setTextSize(h * 0.035f);
    }

    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(Color.rgb(20, 24, 32));
        if (frameDirty) {
            synchronized (frame) { bmp.setPixels(frame, 0, Native.W, 0, 0, Native.W, Native.H); frameDirty = false; }
        }
        c.drawBitmap(bmp, null, screen, bmpPaint);

        // D-pad
        paint.setColor(Color.argb(90, 255, 255, 255));
        c.drawCircle(dpadX, dpadY, dpadR, paint);
        paint.setColor(Color.argb(160, 255, 255, 255));
        float a = dpadR * 0.55f;
        int b = buttons;
        drawArrow(c, dpadX, dpadY - a, (b & Native.UP) != 0);
        drawArrow(c, dpadX, dpadY + a, (b & Native.DOWN) != 0);
        drawArrow(c, dpadX - a, dpadY, (b & Native.LEFT) != 0);
        drawArrow(c, dpadX + a, dpadY, (b & Native.RIGHT) != 0);

        for (Btn bt : btns) {
            boolean down = (b & bt.bit) != 0;
            paint.setColor(down ? Color.argb(220, 120, 200, 255) : Color.argb(110, 255, 255, 255));
            if (bt.round) c.drawOval(bt.r, paint);
            else c.drawRoundRect(bt.r, 16, 16, paint);
            c.drawText(bt.label, bt.r.centerX(), bt.r.centerY() + text.getTextSize() / 3, text);
        }
        paint.setColor(Color.argb(110, 255, 255, 255));
        c.drawRoundRect(menuRect, 16, 16, paint);
        c.drawText("MENU", menuRect.centerX(), menuRect.centerY() + text.getTextSize() / 3, text);

        if (fps > 0) {
            text.setTextAlign(Paint.Align.LEFT);
            c.drawText(String.format("%.0f fps  %s", fps, coreName), 12, text.getTextSize() + 8, text);
            text.setTextAlign(Paint.Align.CENTER);
        }
        if (message != null) {
            paint.setColor(Color.argb(200, 0, 0, 0));
            c.drawRect(screen, paint);
            float y = screen.centerY() - text.getTextSize();
            for (String line : message.split("\n")) { c.drawText(line, screen.centerX(), y, text); y += text.getTextSize() * 1.4f; }
        }
        if (console != null) {
            paint.setColor(Color.argb(230, 0, 0, 0));
            c.drawRect(0, 0, getWidth(), getHeight(), paint);
            Paint t = new Paint(text);
            t.setTextAlign(Paint.Align.LEFT);
            t.setTextSize(getHeight() * 0.025f);
            String[] lines = console.split("\n");
            int max = (int) (getHeight() / (t.getTextSize() * 1.2f));
            float y = t.getTextSize();
            for (int i = Math.max(0, lines.length - max); i < lines.length; i++) { c.drawText(lines[i], 8, y, t); y += t.getTextSize() * 1.2f; }
        }
    }

    private void drawArrow(Canvas c, float x, float y, boolean down) {
        paint.setColor(down ? Color.argb(230, 120, 200, 255) : Color.argb(150, 255, 255, 255));
        c.drawCircle(x, y, dpadR * 0.22f, paint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (console != null) { if (action == MotionEvent.ACTION_DOWN) { console = null; invalidate(); } return true; }
        if (action == MotionEvent.ACTION_DOWN) {
            float x = e.getX(), y = e.getY();
            if (message != null && screen.contains((int) x, (int) y)) { act.pickNand(); return true; }
            if (menuRect.contains(x, y)) { showMenu(); return true; }
        }
        int mask = 0;
        for (int i = 0; i < e.getPointerCount(); i++) {
            if ((action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP) && i == e.getActionIndex()) continue;
            if (action == MotionEvent.ACTION_CANCEL) continue;
            float x = e.getX(i), y = e.getY(i);
            float dx = x - dpadX, dy = y - dpadY;
            if (dx * dx + dy * dy <= dpadR * dpadR * 1.3f && Math.hypot(dx, dy) > dpadR * 0.15f) {
                double ang = Math.toDegrees(Math.atan2(dy, dx));
                if (ang > -67.5 && ang < 67.5) mask |= Native.RIGHT;
                if (ang > 112.5 || ang < -112.5) mask |= Native.LEFT;
                if (ang > 22.5 && ang < 157.5) mask |= Native.DOWN;
                if (ang < -22.5 && ang > -157.5) mask |= Native.UP;
            }
            for (Btn bt : btns) {
                RectF r = new RectF(bt.r);
                r.inset(-12, -12);
                if (r.contains(x, y)) mask |= bt.bit;
            }
        }
        if (mask != buttons) { buttons = mask; invalidate(); }
        return true;
    }

    private void showMenu() {
        new AlertDialog.Builder(getContext())
                .setTitle("PocketSprout")
                .setItems(new String[] {"Insert cartridge", "Eject cartridge", "Reset", "Show serial console",
                        act.jitWanted() ? "CPU core: JIT (tap to use interpreter)" : "CPU core: interpreter (tap to use JIT)", "Load NAND dump"}, (d, which) -> {
                    if (which == 0) act.pickCart();
                    else if (which == 1) act.ejectCart();
                    else if (which == 2) act.resetEmu();
                    else if (which == 3) act.showConsole();
                    else if (which == 4) act.toggleJit();
                    else act.pickNand();
                }).show();
    }
}
