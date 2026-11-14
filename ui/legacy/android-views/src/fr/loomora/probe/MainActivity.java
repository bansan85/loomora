package fr.loomora.probe;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.text.Editable;
import android.text.InputFilter;
import android.text.Spanned;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewTreeObserver;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Legacy UI of Android (API 9 and later): a text field pre-filled "Coucou" limited to 1,000
 * characters, a label with the greeting computed by the backend, the end-of-life warning, and the
 * probe mode. Plain framework views built in code, no resources, no AndroidX. No application
 * logic: the greeting, the end-of-life verdict and the probe line all come from Native.
 */
public class MainActivity extends Activity {
    private static final int MAX_CHARS = 1000;
    private static final int EXPECTED_ABI = 1; // LOOMORA_ABI_VERSION
    private static final int FAMILY_ANDROID = 4; // LOOMORA_FAMILY_ANDROID
    private static final long GIVE_UP_MS = 10000L; // probe: emitted anyway if nothing was drawn

    private final Handler handler = new Handler();
    private long launchMs;
    private boolean probe;
    private boolean displayed;
    private int displayMs;
    private boolean emitted;
    private boolean eolShown;
    private EditText field;
    private TextView greeting;

    /** Keeps at most MAX_CHARS UTF-16 units and never cuts a surrogate pair in two. */
    private static final class MaxChars implements InputFilter {
        public CharSequence filter(CharSequence source, int start, int end, Spanned dest,
                int dstart, int dend) {
            int keep = MAX_CHARS - (dest.length() - (dend - dstart));
            if (keep <= 0) {
                return "";
            }
            if (keep >= end - start) {
                return null;
            }
            keep += start;
            if (Character.isHighSurrogate(source.charAt(keep - 1))) {
                keep--;
                if (keep == start) {
                    return "";
                }
            }
            return source.subSequence(start, keep);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        launchMs = SystemClock.elapsedRealtime();
        super.onCreate(savedInstanceState);
        probe = Probe.enabled(getIntent());

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        field = new EditText(this);
        field.setId(1); // an id lets the field keep its text when the system recreates the activity
        greeting = new TextView(this);
        greeting.setGravity(Gravity.CENTER);
        greeting.setTextSize(24);

        boolean ready = Native.LOADED && Native.abiVersion() == EXPECTED_ABI;
        if (!ready) {
            greeting.setText(Native.LOADED
                    ? "The backend does not match this UI (ABI version " + Native.abiVersion()
                            + ", expected " + EXPECTED_ABI + ")."
                    : "The backend could not be loaded: " + Native.LOAD_ERROR);
        } else {
            int[] v = Probe.androidVersion();
            if (Native.isEol(FAMILY_ANDROID, v[0], v[1]) == 1) {
                TextView banner = new TextView(this);
                banner.setText(Native.eolWarning());
                banner.setBackgroundColor(0xFFFFF3CD);
                banner.setTextColor(Color.BLACK);
                banner.setPadding(pad / 2, pad / 2, pad / 2, pad / 2);
                root.addView(banner, new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT));
                eolShown = true;
            }
        }
        field.setFilters(new InputFilter[] {new MaxChars()});
        field.setSingleLine(true);
        root.addView(field, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(greeting, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);
        setTitle("Loomora");

        if (ready) {
            field.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                }

                public void onTextChanged(CharSequence s, int start, int before, int count) {
                }

                public void afterTextChanged(Editable s) {
                    updateGreeting();
                }
            });
            String override = probe ? Probe.inputOverride(getIntent()) : null;
            field.setText(override != null ? override : "Coucou");
            updateGreeting();
        }

        if (probe) {
            final View content = root;
            content.getViewTreeObserver().addOnPreDrawListener(
                    new ViewTreeObserver.OnPreDrawListener() {
                        public boolean onPreDraw() {
                            content.getViewTreeObserver().removeOnPreDrawListener(this);
                            displayed = true;
                            displayMs = (int) (SystemClock.elapsedRealtime() - launchMs);
                            handler.postDelayed(emitAndFinish, Probe.EXIT_DELAY_MS);
                            return true;
                        }
                    });
            handler.postDelayed(emitAndFinish, GIVE_UP_MS);
        }
    }

    private void updateGreeting() {
        String text = Native.greet(field.getText().toString());
        greeting.setText(text != null ? text : "The greeting could not be computed.");
    }

    private final Runnable emitAndFinish = new Runnable() {
        public void run() {
            if (emitted) {
                return;
            }
            emitted = true;
            if (Native.LOADED) {
                Probe.emit(field.getText().toString(), greeting.getText().toString(), displayed,
                        displayMs, eolShown, "");
            }
            finish();
        }
    };

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(emitAndFinish);
        super.onDestroy();
    }
}
