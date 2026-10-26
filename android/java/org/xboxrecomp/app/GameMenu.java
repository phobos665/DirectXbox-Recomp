package org.xboxrecomp.app;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/*
 * The in-game menu: a panel down the right of the screen, opened with Back.
 * Resume, Settings and Quit game; Settings are the ones the runtime can
 * change while the title runs, plus two read at start, named "(requires
 * restart)".
 *
 * Plain views, built in code (the APK has no layout resources), focusable so
 * a pad's d-pad moves through them, A chooses and B goes back. While it is
 * open the title is held at its next frame (RecompActivity.nativeMenu), so
 * the last frame stays on screen behind it.
 */
final class GameMenu {
    interface Host {
        String settings();                    // key=value;...
        void set(String key, String value);
        void opened(boolean open);
        void quit();
    }

    private static final int PANEL = 0xF0121519;
    private static final int ROW = 0x00000000;
    private static final int ROW_FOCUS = 0x33FFFFFF;
    private static final int TEXT = 0xFFE8EAED;
    private static final int MUTED = 0xFF9AA3AD;
    private static final int ACCENT = 0xFF3FB37A;

    private static final String[] CAPS = { "adaptive", "60", "30", "off" };
    private static final String[] CAP_NAMES = { "Adaptive", "60 fps", "30 fps", "Off" };
    private static final String[] TOUCH = { "auto", "on", "off" };
    private static final String[] TOUCH_NAMES = { "Automatic", "On", "Off" };

    private final Activity activity;
    private final Host host;
    private final FrameLayout root;
    private final LinearLayout panel;
    private boolean open;
    private boolean inSubPage;      // Settings or the quit question: Back returns to the main page
    private Map<String, String> values = new HashMap<>();

    GameMenu(Activity activity, Host host) {
        this.activity = activity;
        this.host = host;
        root = new FrameLayout(activity);
        root.setBackgroundColor(0x66000000);
        root.setVisibility(View.GONE);
        root.setOnClickListener(v -> close());      // a tap beside the panel closes it
        panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(PANEL);
        panel.setClickable(true);                    // taps on the panel stay there
        int pad = dp(20);
        panel.setPadding(pad, pad, pad, pad);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
            Math.min(dp(420), (int)(activity.getResources().getDisplayMetrics().widthPixels * 0.42f)),
            ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END);
        root.addView(panel, lp);
        activity.addContentView(root, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    boolean isOpen() { return open; }

    void toggle() {
        if (open) {
            if (inSubPage) showMain();
            else close();
        } else {
            openMenu();
        }
    }

    void openMenu() {
        open = true;
        host.opened(true);
        showMain();
        root.setVisibility(View.VISIBLE);
        root.bringToFront();
    }

    void close() {
        if (!open) return;
        open = false;
        root.setVisibility(View.GONE);
        host.opened(false);
    }

    /* A pad, while the menu is open: the d-pad moves focus as Android does,
     * A presses, B goes back a page. Everything is kept from the game. */
    boolean onKey(KeyEvent e) {
        if (!open) return false;
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_B) {
            if (e.getAction() == KeyEvent.ACTION_UP) toggle();
            return true;
        }
        if (code == KeyEvent.KEYCODE_BUTTON_A) {
            KeyEvent center = new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(),
                                           KeyEvent.KEYCODE_DPAD_CENTER, e.getRepeatCount());
            return root.dispatchKeyEvent(center) || true;
        }
        if (code == KeyEvent.KEYCODE_BUTTON_START) {
            if (e.getAction() == KeyEvent.ACTION_UP) close();
            return true;
        }
        root.dispatchKeyEvent(e);
        return true;
    }

    // ---- pages ----

    private void showMain() {
        inSubPage = false;
        panel.removeAllViews();
        panel.addView(title("Paused"));
        View resume = button("Resume", v -> close());
        panel.addView(resume);
        panel.addView(button("Settings", v -> showSettings()));
        panel.addView(button("Quit game", v -> showQuit()));
        resume.requestFocus();
    }

    /* Asked first: a quit loses whatever the game has not saved, and Quit
     * game sits one press below Settings. Keep playing has the focus, so a
     * stray A press is harmless. */
    private void showQuit() {
        inSubPage = true;
        panel.removeAllViews();
        panel.addView(title("Quit game?"));
        TextView why = text("Anything you haven't saved in the game will be lost.", 15, MUTED, false);
        why.setPadding(dp(12), 0, dp(12), dp(16));
        panel.addView(why);
        View keep = button("Keep playing", v -> showMain());
        panel.addView(keep);
        panel.addView(button("Quit", v -> host.quit()));
        keep.requestFocus();
    }

    private void showSettings() {
        inSubPage = true;
        values = parse(host.settings());
        panel.removeAllViews();
        panel.addView(title("Settings"));
        ScrollView scroll = new ScrollView(activity);
        LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);
        panel.addView(scroll, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        list.addView(section("Display"));
        View first = toggleRow(list, "Frame rate counter", null, "fps_overlay");
        choiceRow(list, "Frame rate limit", null, "frame_cap", CAPS, CAP_NAMES);
        // Read when the game starts, so a change shows from the next start.
        toggleRow(list, "Widescreen (requires restart)", null, "widescreen");
        choiceRow(list, "Resolution (requires restart)", null,
                  "resolution_scale", new String[] { "1", "2", "3", "4" },
                  new String[] { "Native (1x)", "2x", "3x", "4x" });

        list.addView(section("Touch controls"));
        choiceRow(list, "Show", "Automatic: shown with no controller connected",
                  "touch_controls", TOUCH, TOUCH_NAMES);
        sliderRow(list, "Opacity", "touch_opacity", 10, 100);
        sliderRow(list, "Size", "touch_scale", 50, 200);

        panel.addView(button("Back", v -> showMain()));
        first.requestFocus();
    }

    // ---- rows ----

    private View toggleRow(LinearLayout list, String name, String note, final String key) {
        final TextView value = valueText(on(key) ? "On" : "Off");
        View row = row(name, note, value);
        row.setOnClickListener(v -> {
            boolean now = !on(key);
            values.put(key, now ? "1" : "0");
            value.setText(now ? "On" : "Off");
            host.set(key, now ? "1" : "0");
        });
        list.addView(row);
        return row;
    }

    private View choiceRow(LinearLayout list, String name, String note, final String key,
                           final String[] keys, final String[] names) {
        final TextView value = valueText(names[indexOf(keys, values.get(key))]);
        View row = row(name, note, value);
        row.setOnClickListener(v -> {
            int next = (indexOf(keys, values.get(key)) + 1) % keys.length;
            values.put(key, keys[next]);
            value.setText(names[next]);
            host.set(key, keys[next]);
        });
        list.addView(row);
        return row;
    }

    private void sliderRow(LinearLayout list, String name, final String key, final int min, final int max) {
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(12), dp(8), dp(12), dp(8));
        final TextView label = text(name, 16, TEXT, false);
        box.addView(label);
        final SeekBar bar = new SeekBar(activity);
        bar.setMax(max - min);
        int now = Math.round(parseFloat(values.get(key), 1f) * 100f);
        bar.setProgress(Math.max(0, Math.min(max - min, now - min)));
        label.setText(name + "  " + now + "%");
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                int pct = p + min;
                label.setText(name + "  " + pct + "%");
                String v = String.valueOf(pct / 100f);
                values.put(key, v);
                host.set(key, v);
            }
            @Override public void onStartTrackingTouch(SeekBar s) { }
            @Override public void onStopTrackingTouch(SeekBar s) { }
        });
        box.addView(bar);
        list.addView(box);
    }

    private View row(String name, String note, TextView value) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        row.setFocusable(true);
        row.setClickable(true);
        row.setBackground(rowBackground());
        LinearLayout words = new LinearLayout(activity);
        words.setOrientation(LinearLayout.VERTICAL);
        words.addView(text(name, 16, TEXT, false));
        if (note != null) words.addView(text(note, 12, MUTED, false));
        row.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(value);
        return row;
    }

    private View button(String name, View.OnClickListener click) {
        TextView b = text(name, 18, TEXT, true);
        b.setPadding(dp(14), dp(14), dp(14), dp(14));
        b.setFocusable(true);
        b.setClickable(true);
        b.setBackground(rowBackground());
        b.setOnClickListener(click);
        return b;
    }

    private TextView title(String s) {
        TextView t = text(s, 26, TEXT, true);
        t.setPadding(dp(12), 0, dp(12), dp(14));
        return t;
    }

    private TextView section(String s) {
        TextView t = text(s.toUpperCase(), 12, MUTED, true);
        t.setLetterSpacing(0.08f);
        t.setPadding(dp(12), dp(16), dp(12), dp(4));
        return t;
    }

    private TextView valueText(String s) {
        return text(s, 15, ACCENT, true);
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(activity);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private StateListDrawable rowBackground() {
        StateListDrawable d = new StateListDrawable();
        GradientDrawable focus = new GradientDrawable();
        focus.setColor(ROW_FOCUS);
        focus.setCornerRadius(dp(10));
        GradientDrawable rest = new GradientDrawable();
        rest.setColor(ROW);
        d.addState(new int[] { android.R.attr.state_focused }, focus);
        d.addState(new int[] { android.R.attr.state_pressed }, focus);
        d.addState(new int[] { }, rest);
        return d;
    }

    // ---- values ----

    private boolean on(String key) { return "1".equals(values.get(key)); }

    private static int indexOf(String[] keys, String v) {
        for (int i = 0; i < keys.length; i++)
            if (keys[i].equals(v)) return i;
        return 0;
    }

    private static float parseFloat(String s, float fallback) {
        try { return s == null ? fallback : Float.parseFloat(s); } catch (NumberFormatException e) { return fallback; }
    }

    private static Map<String, String> parse(String s) {
        Map<String, String> m = new HashMap<>();
        if (s == null) return m;
        for (String kv : s.split(";")) {
            int eq = kv.indexOf('=');
            if (eq > 0) m.put(kv.substring(0, eq), kv.substring(eq + 1));
        }
        return m;
    }

    private int dp(int v) {
        return Math.round(v * activity.getResources().getDisplayMetrics().density);
    }
}
