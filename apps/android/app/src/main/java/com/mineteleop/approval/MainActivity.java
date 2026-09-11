package com.mineteleop.approval;

import android.Manifest;
import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.*;
import android.text.InputType;
import android.view.*;
import android.widget.*;

/** Native Android views: no browser, WebView or bundled web application. */
public final class MainActivity extends Activity implements ApprovalService.Listener {
    private static final int GREEN = Color.rgb(23, 102, 83), INK = Color.rgb(23, 39, 34), MUTED = Color.rgb(96, 114, 106);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private ApprovalService service;
    private boolean bound, showingInbox;
    private LinearLayout content, cards;
    private TextView status, originLabel, loginOriginLabel;
    private EditText passwordInput;
    private Button loginButton, logoutButton, settingsButton;
    private String lastCards = "";
    private final java.util.Map<TextView, ApprovalRequest> countdowns = new java.util.HashMap<>();
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            service = ((ApprovalService.LocalBinder) binder).service(); service.addListener(MainActivity.this);
        }
        @Override public void onServiceDisconnected(ComponentName name) { service = null; showLogin(); status.setText("值守已停止，请重新登录"); }
    };
    private final Runnable tick = new Runnable() {
        @Override public void run() { if (showingInbox && service != null) renderCards(); handler.postDelayed(this, 1000); }
    };
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        showLogin();
    }
    @Override protected void onStart() {
        super.onStart(); bound = bindService(new Intent(this, ApprovalService.class), connection, BIND_AUTO_CREATE); handler.post(tick);
    }
    @Override protected void onStop() {
        handler.removeCallbacks(tick);
        if (service != null) service.removeListener(this);
        if (bound) { unbindService(connection); bound = false; }
        service = null; super.onStop();
    }
    @Override public void changed() {
        if (service == null) return;
        ApprovalService.Snapshot state = service.snapshot();
        if (state.authenticated != showingInbox) { if (state.authenticated) showInbox(); else showLogin(); }
        status.setText(state.message);
        if (showingInbox) {
            originLabel.setText(getString(R.string.duty_origin, state.origin));
            logoutButton.setEnabled(!state.busy); renderCards();
        } else {
            loginButton.setEnabled(!state.busy); loginButton.setText(state.busy ? "正在处理…" : "登录并开始值守");
            settingsButton.setEnabled(!state.busy); passwordInput.setEnabled(!state.busy);
        }
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private void shell(String eyebrow, String title, String subtitle) {
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true); scroll.setBackgroundColor(Color.WHITE);
        content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL); content.setPadding(dp(24), dp(26), dp(24), dp(24));
        scroll.addView(content);
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            } else view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(scroll);
        addText(content, eyebrow, 12, GREEN, true, 16);
        addText(content, title, 30, INK, true, 10);
        addText(content, subtitle, 15, MUTED, false, 24);
        status = addText(content, "", 14, GREEN, false, 18); status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
    }
    private void showLogin() {
        showingInbox = false;
        shell("MINE TELEOP  /  手机授权", "每次接管\n由你确认", "核对车辆与申请人，同意后控制端自动连接。");
        android.content.SharedPreferences preferences = getSharedPreferences("login_preferences", MODE_PRIVATE);
        loginOriginLabel = addText(content, "", 13, MUTED, false, 18);
        updateLoginOrigin();
        settingsButton = button("连接设置", false);
        settingsButton.setOnClickListener(v -> showConnectionSettings());
        passwordInput = input("密码", "请输入密码", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        passwordInput.setSaveEnabled(false);
        loginButton = button("登录并开始值守", true); content.addView(loginButton, fullWidth(16));
        loginButton.setOnClickListener(view -> {
            if (service == null) { status.setText("正在准备服务，请稍后重试"); return; }
            try {
                String origin = ApiOrigin.normalize(configuredOrigin(), BuildConfig.DEBUG);
                String password = passwordInput.getText().toString();
                if (password.isEmpty()) { status.setText("请填写云端配置的审批密码"); return; }
                preferences.edit().putString("origin", origin).remove("account").apply();
                if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED &&
                        !preferences.getBoolean("notification_asked", false)) {
                    preferences.edit().putBoolean("notification_asked", true).apply();
                    requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
                }
                passwordInput.setText(""); service.login(origin, password);
            } catch (IllegalArgumentException error) { status.setText(error.getMessage()); }
        });
        content.addView(settingsButton, fullWidth(16));
        addText(content, "开启值守后，App 会持续检查申请并显示通知。请允许通知；结束值守请在 App 内退出。", 13, MUTED, false, 0);
    }
    private String configuredOrigin() {
        String saved = getSharedPreferences("login_preferences", MODE_PRIVATE).getString("origin", "");
        return saved.isEmpty() ? ApiOrigin.DEFAULT : saved;
    }
    private void updateLoginOrigin() {
        loginOriginLabel.setText(getString(R.string.configured_origin, configuredOrigin()));
    }
    private void showConnectionSettings() {
        SharedPreferences preferences = getSharedPreferences("login_preferences", MODE_PRIVATE);
        EditText field = new EditText(this); field.setSingleLine(true); field.setTextSize(16);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        field.setHint(ApiOrigin.DEFAULT); field.setContentDescription("云端地址");
        field.setText(configuredOrigin());
        LinearLayout frame = new LinearLayout(this); frame.setPadding(dp(24), dp(12), dp(24), 0); frame.addView(field, fullWidth(0));
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("连接设置").setMessage("默认服务器：60.205.213.254，使用其 HTTPS 域名连接。")
                .setView(frame).setNegativeButton("取消", null).setNeutralButton("恢复默认", null).setPositiveButton("保存", null).create();
        dialog.setOnShowListener(v -> {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(button -> field.setText(ApiOrigin.DEFAULT));
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(button -> {
                try { preferences.edit().putString("origin", ApiOrigin.normalize(field.getText().toString(), BuildConfig.DEBUG)).apply(); updateLoginOrigin(); dialog.dismiss(); }
                catch (IllegalArgumentException error) { field.setError(error.getMessage()); }
            });
        }); dialog.show();
    }
    private void showInbox() {
        showingInbox = true; lastCards = "";
        shell("MINE TELEOP  /  审批台", "车辆控制申请", "同意仅对当前申请有效；拒绝或超时不会建立连接。");
        originLabel = addText(content, "", 13, MUTED, false, 18);
        if (!getSystemService(NotificationManager.class).areNotificationsEnabled()) {
            Button settings = button("通知未开启 · 前往设置", false); content.addView(settings, fullWidth(16));
            settings.setOnClickListener(v -> startActivity(new Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                    .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE, getPackageName())));
        }
        cards = new LinearLayout(this); cards.setOrientation(LinearLayout.VERTICAL); content.addView(cards);
        logoutButton = button("退出值守", false); content.addView(logoutButton, fullWidth(12));
        logoutButton.setOnClickListener(v -> { if (service != null) service.logout(); });
        addText(content, "锁屏时通过系统通知提醒。系统休眠、网络中断或强行停止 App 可能延迟提醒；云端超时后自动结束申请。", 12, MUTED, false, 0);
    }
    private void renderCards() {
        ApprovalService.Snapshot state = service.snapshot();
        long now = SystemClock.elapsedRealtime(); StringBuilder key = new StringBuilder().append(state.busy).append(state.connected);
        for (ApprovalRequest request : state.requests) key.append('|').append(request.id).append(request.state).append(request.actionable(now));
        if (lastCards.equals(key.toString())) {
            for (java.util.Map.Entry<TextView, ApprovalRequest> entry : countdowns.entrySet())
                entry.getKey().setText(getString(R.string.remaining, entry.getValue().remainingSeconds(now)));
            return;
        }
        lastCards = key.toString(); cards.removeAllViews(); countdowns.clear();
        if (state.requests.isEmpty()) {
            addText(cards, state.connected ? "暂无待确认申请" : "连接恢复后将重新获取申请", 20, INK, true, 12);
            addText(cards, "新的控制申请会自动显示在这里。", 14, MUTED, false, 32); return;
        }
        for (ApprovalRequest request : state.requests) {
            LinearLayout card = new LinearLayout(this); card.setOrientation(LinearLayout.VERTICAL); card.setPadding(dp(20), dp(20), dp(20), dp(20));
            GradientDrawable background = new GradientDrawable(); background.setColor(Color.rgb(244, 248, 245)); background.setCornerRadius(dp(18)); card.setBackground(background);
            cards.addView(card, fullWidth(18));
            addText(card, request.vehicle, 24, INK, true, 10);
            addText(card, "申请人  " + request.driver, 15, INK, false, 12);
            boolean pending = request.actionable(now);
            TextView clock = addText(card, "pending".equals(request.state) ? (pending ? "等待确认 · 剩余 " + request.remainingSeconds(now) + " 秒" : "申请已过期") : request.label(), 14, GREEN, true, 12);
            if (pending) countdowns.put(clock, request);
            if (pending) {
                LinearLayout actions = new LinearLayout(this);
                Button reject = button("拒绝", false), approve = button("同意", true);
                LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, dp(52), 1); half.setMarginEnd(dp(12));
                actions.addView(reject, half); actions.addView(approve, new LinearLayout.LayoutParams(0, dp(52), 1)); card.addView(actions);
                reject.setEnabled(state.connected && !state.busy); approve.setEnabled(state.connected && !state.busy);
                reject.setContentDescription("拒绝 " + request.driver + " 接管 " + request.vehicle);
                approve.setContentDescription("同意 " + request.driver + " 接管 " + request.vehicle);
                reject.setOnClickListener(v -> { if (service != null) service.decide(request.id, "reject"); });
                approve.setOnClickListener(v -> { if (service != null) service.decide(request.id, "approve"); });
            }
        }
    }
    private LinearLayout.LayoutParams fullWidth(int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2); params.bottomMargin = dp(bottom); return params;
    }
    private TextView addText(LinearLayout parent, String text, int size, int color, boolean bold, int bottom) {
        TextView view = new TextView(this); view.setText(text); view.setTextSize(size); view.setTextColor(color);
        view.setLineSpacing(dp(3), 1); if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        parent.addView(view, fullWidth(bottom)); return view;
    }
    private EditText input(String label, String hint, int type) {
        TextView caption = addText(content, label, 14, INK, true, 4);
        EditText field = new EditText(this); field.setId(View.generateViewId()); caption.setLabelFor(field.getId());
        field.setHint(hint); field.setInputType(type); field.setSingleLine(true); field.setTextSize(16); field.setMinHeight(dp(54));
        field.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO); content.addView(field, fullWidth(18)); return field;
    }
    private Button button(String text, boolean primary) {
        Button view = new Button(this); view.setText(text); view.setTextSize(16); view.setAllCaps(false); view.setMinHeight(dp(52));
        view.setTextColor(primary ? Color.WHITE : GREEN);
        view.setBackgroundTintList(android.content.res.ColorStateList.valueOf(primary ? GREEN : Color.rgb(232, 239, 235)));
        return view;
    }
}
