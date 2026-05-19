package com.template.lsposed.ui;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import com.template.lsposed.FeatureRegistry;
import com.template.lsposed.FeatureState;
import com.template.lsposed.NativeBridge;
import com.template.lsposed.TemplateConfig;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Floating in-Activity overlay.
 *
 * <p>The style intentionally mirrors the dark/lavender Nyx panel from the current module. The small
 * oval tag is movable. When opened, the rectangular panel is also movable by dragging the header.</p>
 */
public final class OverlayController {
    private static final Object LOCK = new Object();
    private static OverlayController INSTANCE;

    private final Application app;
    private final Context appCtx;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private Activity currentActivity;
    private Activity attachedActivity;
    private View tagView;
    private View panelView;
    private View panelDragHandle;
    private TextView panelStats;
    private FeatureRegistry.Listener panelListener;
    private final Map<String, Button> toggleRefs = new HashMap<>();

    private OverlayController(Application app) {
        this.app = app;
        this.appCtx = app;
    }

    public static void attach(Context context) {
        if (context == null) return;
        Context appContext = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        if (!(appContext instanceof Application)) {
            if (TemplateConfig.VERBOSE_LOGS) {
                Log.w(TemplateConfig.LOG_TAG, "Overlay attach skipped: context is not an Application");
            }
            return;
        }
        FeatureRegistry.initialize(appContext);
        synchronized (LOCK) {
            if (INSTANCE != null) return;
            INSTANCE = new OverlayController((Application) appContext);
            INSTANCE.start();
        }
    }

    private void start() {
        app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityCreated(Activity activity, Bundle savedInstanceState) {}
            @Override public void onActivityStarted(Activity activity) {}
            @Override public void onActivityResumed(Activity activity) {
                currentActivity = activity;
                ensureOverlayAlive();
            }
            @Override public void onActivityPaused(Activity activity) {
                if (currentActivity == activity) currentActivity = null;
                hideOverlay(activity);
            }
            @Override public void onActivityStopped(Activity activity) {}
            @Override public void onActivitySaveInstanceState(Activity activity, Bundle outState) {}
            @Override public void onActivityDestroyed(Activity activity) {
                if (currentActivity == activity) currentActivity = null;
                hideOverlay(activity);
            }
        });

        Runnable retry = this::ensureOverlayAlive;
        mainHandler.postDelayed(retry, 700);
        mainHandler.postDelayed(retry, 1800);
        mainHandler.postDelayed(retry, 4000);
        mainHandler.postDelayed(new Runnable() {
            @Override public void run() {
                ensureOverlayAlive();
                mainHandler.postDelayed(this, 2500);
            }
        }, 2500);
    }

    private void ensureOverlayAlive() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(this::ensureOverlayAlive);
            return;
        }
        if (currentActivity == null) currentActivity = resolveCurrentActivityViaReflection();
        Activity target = currentActivity;
        if (target == null) return;

        ViewGroup root = (ViewGroup) target.getWindow().getDecorView();
        boolean staleParent = tagView != null && tagView.getParent() != root;
        if (attachedActivity != target || tagView == null || tagView.getParent() == null || staleParent) {
            detachFromParent(tagView);
            detachFromParent(panelView);
            tagView = null;
            panelView = null;
            panelDragHandle = null;
            attachedActivity = null;
            closePanelResources();
            showOverlay(target);
            return;
        }
        lift(tagView);
        lift(panelView);
    }

    private void showOverlay(Activity target) {
        try {
            ViewGroup root = (ViewGroup) target.getWindow().getDecorView();
            tagView = buildTagView();
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(48), dp(48));
            lp.leftMargin = dp(12);
            lp.topMargin = dp(120);
            root.addView(tagView, lp);
            lift(tagView);
            makeDraggable(tagView, tagView, lp, true);
            attachedActivity = target;
            if (TemplateConfig.VERBOSE_LOGS) {
                Log.i(TemplateConfig.LOG_TAG, "Overlay attached to " + target.getClass().getName());
            }
        } catch (Throwable t) {
            if (TemplateConfig.VERBOSE_LOGS) Log.e(TemplateConfig.LOG_TAG, "Overlay attach failed", t);
        }
    }

    private View buildTagView() {
        TextView t = new TextView(appCtx);
        t.setText(TemplateConfig.MENU_BUBBLE_TEXT);
        t.setTextColor(Color.WHITE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setGravity(Gravity.CENTER);
        t.setTypeface(t.getTypeface(), Typeface.BOLD);
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        bg.setColor(Color.argb(0xCC, 0x3A, 0x2D, 0x6B));
        bg.setStroke(dp(1), Color.argb(0xFF, 0xB8, 0x9A, 0xFF));
        t.setBackground(bg);
        t.setOnClickListener(v -> togglePanel());
        return t;
    }

    private void togglePanel() {
        if (panelView != null) {
            detachFromParent(panelView);
            panelView = null;
            panelDragHandle = null;
            closePanelResources();
            return;
        }
        if (currentActivity == null) currentActivity = resolveCurrentActivityViaReflection();
        Activity target = currentActivity != null ? currentActivity : attachedActivity;
        if (target == null) return;

        ViewGroup root = (ViewGroup) target.getWindow().getDecorView();
        panelView = buildPanelView();
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(280), FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.CENTER;
        root.addView(panelView, lp);
        lift(panelView);
        if (panelDragHandle != null) {
            makeDraggable(panelDragHandle, panelView, lp, false);
        } else {
            makeDraggable(panelView, panelView, lp, false);
        }
    }

    private View buildPanelView() {
        toggleRefs.clear();

        LinearLayout root = new LinearLayout(appCtx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(18), dp(18), dp(18));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(0xE0, 0x0E, 0x0E, 0x14));
        bg.setCornerRadius(dp(16));
        bg.setStroke(dp(1), Color.argb(0xFF, 0x6B, 0x5B, 0xFF));
        root.setBackground(bg);

        LinearLayout header = new LinearLayout(appCtx);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setPadding(0, 0, 0, dp(8));
        panelDragHandle = header;

        TextView title = new TextView(appCtx);
        title.setText(TemplateConfig.MENU_TITLE);
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        title.setTypeface(title.getTypeface(), Typeface.BOLD);
        header.addView(title);

        TextView sub = new TextView(appCtx);
        sub.setText(TemplateConfig.MENU_SUBTITLE);
        sub.setTextColor(Color.argb(0xFF, 0xB8, 0x9A, 0xFF));
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        sub.setTypeface(sub.getTypeface(), Typeface.BOLD);
        header.addView(sub);
        root.addView(header);

        View hairline = new View(appCtx);
        LinearLayout.LayoutParams hp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1));
        hp.bottomMargin = dp(8);
        hairline.setBackgroundColor(Color.argb(0x55, 0xB8, 0x9A, 0xFF));
        root.addView(hairline, hp);

        ColorStateList lavender = ColorStateList.valueOf(Color.argb(0xFF, 0xB8, 0x9A, 0xFF));
        ColorStateList trackBg = ColorStateList.valueOf(Color.argb(0xFF, 0x3A, 0x35, 0x50));

        final FeatureRegistry.Feature multiplier = findFloatFeature(FeatureRegistry.KEY_MULTIPLIER);
        final float minMult = multiplier != null ? multiplier.min : 1f;
        final float maxMult = multiplier != null ? multiplier.max : 30f;
        final String multLabelText = multiplier != null ? multiplier.label : "Multiplier";

        final TextView multLabel = new TextView(appCtx);
        multLabel.setTextColor(Color.WHITE);
        multLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        multLabel.setText(formatMultiplier(multLabelText, FeatureState.getMultiplier()));
        root.addView(multLabel);

        final SeekBar multBar = new SeekBar(appCtx);
        int range = (int) Math.round((maxMult - minMult) * 10f);
        multBar.setMax(Math.max(1, range));
        multBar.setProgress(clamp(Math.round((FeatureState.getMultiplier() - minMult) * 10f),
                0, multBar.getMax()));
        multBar.setProgressTintList(lavender);
        multBar.setProgressBackgroundTintList(trackBg);
        multBar.setThumbTintList(lavender);
        root.addView(multBar);

        for (FeatureRegistry.Feature f : FeatureRegistry.features()) {
            if (f.type == FeatureRegistry.Type.BOOL) addToggleRow(root, f);
        }

        final TextView stats = new TextView(appCtx);
        stats.setTextColor(Color.parseColor("#9DA5B4"));
        stats.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        stats.setPadding(0, dp(12), 0, dp(10));
        stats.setText(FeatureState.summary());
        root.addView(stats);
        panelStats = stats;

        multBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float value = minMult + progress / 10f;
                FeatureState.setMultiplier(value);
                multLabel.setText(formatMultiplier(multLabelText, FeatureState.getMultiplier()));
                stats.setText(FeatureState.summary());
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        LinearLayout row = new LinearLayout(appCtx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        rowLp.topMargin = dp(4);
        row.setLayoutParams(rowLp);

        Button reset = nyxButton("Reset");
        reset.setOnClickListener(v -> {
            FeatureRegistry.resetToDefaults();
            multBar.setProgress(clamp(Math.round((FeatureState.getMultiplier() - minMult) * 10f),
                    0, multBar.getMax()));
            multLabel.setText(formatMultiplier(multLabelText, FeatureState.getMultiplier()));
            Toast.makeText(appCtx, "Defaults restored", Toast.LENGTH_SHORT).show();
        });

        Button nativeRetry = nyxButton("Native");
        nativeRetry.setOnClickListener(v -> {
            int result = NativeBridge.installNativeHooks(appCtx, appCtx.getPackageName());
            stats.setText(FeatureState.summary());
            Toast.makeText(appCtx, "Native install: " + result, Toast.LENGTH_SHORT).show();
        });

        Button close = nyxButton("X");
        close.setOnClickListener(v -> togglePanel());

        LinearLayout.LayoutParams flex = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        flex.setMargins(0, 0, dp(4), 0);
        reset.setLayoutParams(flex);
        nativeRetry.setLayoutParams(new LinearLayout.LayoutParams(flex));
        LinearLayout.LayoutParams closeLp = new LinearLayout.LayoutParams(dp(36), LinearLayout.LayoutParams.WRAP_CONTENT);
        close.setLayoutParams(closeLp);

        row.addView(reset);
        row.addView(nativeRetry);
        row.addView(close);
        root.addView(row);

        FeatureRegistry.Listener listener = key -> mainHandler.post(this::refreshPanel);
        panelListener = listener;
        FeatureRegistry.addListener(listener);

        return root;
    }

    private void addToggleRow(LinearLayout parent, FeatureRegistry.Feature feature) {
        LinearLayout row = new LinearLayout(appCtx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(4), 0, dp(4));
        LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        row.setLayoutParams(rowLp);

        TextView label = new TextView(appCtx);
        label.setTextColor(Color.WHITE);
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        label.setText(feature.label);
        label.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams labelLp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 1f);
        label.setLayoutParams(labelLp);
        row.addView(label);

        final Button toggle = nyxButton(FeatureRegistry.getBool(feature.key) ? "ON" : "OFF");
        toggle.setOnClickListener(v -> {
            boolean next = !FeatureRegistry.getBool(feature.key);
            FeatureRegistry.setBool(feature.key, next);
            toggle.setText(next ? "ON" : "OFF");
            if (panelStats != null) panelStats.setText(FeatureState.summary());
        });
        LinearLayout.LayoutParams toggleLp = new LinearLayout.LayoutParams(dp(60), dp(34));
        toggle.setLayoutParams(toggleLp);
        row.addView(toggle);

        parent.addView(row);
        toggleRefs.put(feature.key, toggle);
    }

    private FeatureRegistry.Feature findFloatFeature(String key) {
        for (FeatureRegistry.Feature f : FeatureRegistry.features()) {
            if (f.type == FeatureRegistry.Type.FLOAT && key.equals(f.key)) return f;
        }
        return null;
    }

    private static String formatMultiplier(String label, float value) {
        return String.format(Locale.US, "%s  \u00d7%.1f", label, value);
    }

    private void refreshPanel() {
        if (panelStats != null) panelStats.setText(FeatureState.summary());
        for (Map.Entry<String, Button> e : toggleRefs.entrySet()) {
            Button b = e.getValue();
            if (b != null) b.setText(FeatureRegistry.getBool(e.getKey()) ? "ON" : "OFF");
        }
    }

    private void closePanelResources() {
        panelStats = null;
        toggleRefs.clear();
        if (panelListener != null) {
            FeatureRegistry.removeListener(panelListener);
            panelListener = null;
        }
    }

    private Button nyxButton(String text) {
        Button b = new Button(appCtx);
        b.setText(text);
        b.setAllCaps(true);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        b.setSingleLine(true);
        b.setIncludeFontPadding(false);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(0xFF, 0x1A, 0x1A, 0x24));
        bg.setCornerRadius(dp(8));
        bg.setStroke(dp(1), Color.argb(0xFF, 0x3A, 0x35, 0x50));
        b.setBackground(bg);
        b.setMinimumHeight(dp(36));
        b.setMinHeight(dp(36));
        b.setMinimumWidth(0);
        b.setMinWidth(0);
        b.setPadding(dp(10), dp(6), dp(10), dp(6));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            b.setStateListAnimator(null);
            b.setElevation(0f);
        }
        return b;
    }

    private void makeDraggable(View handle, View movedView, FrameLayout.LayoutParams lp, boolean clickOnTap) {
        final int[] start = new int[2];
        final float[] startTouch = new float[2];
        final boolean[] moved = new boolean[1];
        handle.setOnTouchListener((view, ev) -> {
            switch (ev.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    if (lp.gravity == Gravity.CENTER) {
                        lp.gravity = Gravity.TOP | Gravity.START;
                        lp.leftMargin = movedView.getLeft();
                        lp.topMargin = movedView.getTop();
                        movedView.setLayoutParams(lp);
                    }
                    start[0] = lp.leftMargin;
                    start[1] = lp.topMargin;
                    startTouch[0] = ev.getRawX();
                    startTouch[1] = ev.getRawY();
                    moved[0] = false;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    int dx = (int) (ev.getRawX() - startTouch[0]);
                    int dy = (int) (ev.getRawY() - startTouch[1]);
                    if (Math.abs(dx) > dp(3) || Math.abs(dy) > dp(3)) moved[0] = true;
                    lp.leftMargin = clamp(start[0] + dx, 0, maxLeft(movedView));
                    lp.topMargin = clamp(start[1] + dy, 0, maxTop(movedView));
                    movedView.setLayoutParams(lp);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    if (clickOnTap && !moved[0]) movedView.performClick();
                    return true;
                default:
                    return true;
            }
        });
    }

    private int maxLeft(View movedView) {
        View parent = (View) movedView.getParent();
        if (parent == null || parent.getWidth() <= 0 || movedView.getWidth() <= 0) return Integer.MAX_VALUE / 4;
        return Math.max(0, parent.getWidth() - movedView.getWidth());
    }

    private int maxTop(View movedView) {
        View parent = (View) movedView.getParent();
        if (parent == null || parent.getHeight() <= 0 || movedView.getHeight() <= 0) return Integer.MAX_VALUE / 4;
        return Math.max(0, parent.getHeight() - movedView.getHeight());
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private void hideOverlay(Activity activity) {
        mainHandler.post(() -> {
            if (attachedActivity == null) return;
            if (activity != null && attachedActivity != activity) return;
            detachFromParent(tagView);
            detachFromParent(panelView);
            tagView = null;
            panelView = null;
            panelDragHandle = null;
            attachedActivity = null;
            closePanelResources();
        });
    }

    private static void detachFromParent(View view) {
        if (view == null) return;
        if (view.getParent() instanceof ViewGroup) {
            ((ViewGroup) view.getParent()).removeView(view);
        }
    }

    private void lift(View view) {
        if (view == null) return;
        view.setVisibility(View.VISIBLE);
        view.setAlpha(1f);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            view.setElevation(dp(64));
            view.setTranslationZ(dp(64));
        }
        view.bringToFront();
    }

    private static Activity resolveCurrentActivityViaReflection() {
        try {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Object at = atClass.getMethod("currentActivityThread").invoke(null);
            java.lang.reflect.Field activitiesField = atClass.getDeclaredField("mActivities");
            activitiesField.setAccessible(true);
            Object activitiesObj = activitiesField.get(at);
            if (!(activitiesObj instanceof java.util.Map)) return null;
            java.util.Map<?, ?> activities = (java.util.Map<?, ?>) activitiesObj;
            for (Object record : activities.values()) {
                if (record == null) continue;
                Class<?> recClass = record.getClass();
                java.lang.reflect.Field pausedField;
                try {
                    pausedField = recClass.getDeclaredField("paused");
                } catch (NoSuchFieldException ignored) {
                    continue;
                }
                pausedField.setAccessible(true);
                if (pausedField.getBoolean(record)) continue;
                java.lang.reflect.Field activityField = recClass.getDeclaredField("activity");
                activityField.setAccessible(true);
                Object activity = activityField.get(record);
                if (activity instanceof Activity) return (Activity) activity;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private int dp(float value) {
        return (int) (value * appCtx.getResources().getDisplayMetrics().density);
    }
}
