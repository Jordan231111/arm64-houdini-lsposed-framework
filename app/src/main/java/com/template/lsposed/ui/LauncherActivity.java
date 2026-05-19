package com.template.lsposed.ui;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.text.method.LinkMovementMethod;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.ScrollView;
import android.widget.TextView;

import com.template.lsposed.TemplateConfig;

/** Simple launcher screen so the APK is easy to identify after install. */
public final class LauncherActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        TextView text = new TextView(this);
        text.setTextColor(Color.rgb(230, 230, 240));
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        text.setGravity(Gravity.START);
        text.setPadding(dp(18), dp(18), dp(18), dp(18));
        text.setLinksClickable(true);
        text.setMovementMethod(LinkMovementMethod.getInstance());
        text.setText("LSPosed Universal Template\n\n"
                + "1. Edit TemplateConfig.TARGET_PACKAGES.\n"
                + "2. Edit META-INF/xposed/scope.list and res/values/arrays.xml.\n"
                + "3. For native work, set TemplateConfig.NATIVE_TARGET_LIBRARIES on a game branch.\n"
                + "4. Build: ./gradlew :app:assembleRelease\n"
                + "5. Install and enable this module in LSPosed / Vector.\n\n"
                + "Current first target: " + TemplateConfig.TARGET_PACKAGES[0] + "\n\n"
                + "The in-app Nyx bubble appears inside the target Activity after Application.attach. "
                + "Tap it to open the rectangular panel; drag the panel header to move it.");

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(14, 14, 20));
        scroll.addView(text);
        setContentView(scroll);
    }

    private int dp(float value) {
        return (int) (value * getResources().getDisplayMetrics().density);
    }
}
