package com.mineteleop.approval;

import android.app.*;
import android.content.Intent;
import android.os.*;
import org.json.*;
import java.util.*;
import java.util.concurrent.*;

/** Credentials live only for the lifetime of the user-started duty service. */
public class ApprovalService extends Service {
    static final String DUTY = "approval_duty", REQUESTS = "approval_requests";
    private static final int DUTY_ID = 1, REQUEST_ID = 2;
    static final long SYNC_TIMEOUT_MS = 30000;
    interface Listener { void changed(); }
    final class LocalBinder extends Binder { ApprovalService service() { return ApprovalService.this; } }
    static final class Snapshot {
        final boolean authenticated, busy, connected;
        final String origin, message;
        final List<ApprovalRequest> requests;
        Snapshot(boolean authenticated, boolean busy, boolean connected, String origin,
                 String message, List<ApprovalRequest> requests) {
            this.authenticated = authenticated; this.busy = busy; this.connected = connected;
            this.origin = origin; this.message = message;
            this.requests = Collections.unmodifiableList(new ArrayList<>(requests));
        }
    }
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private final Set<Listener> listeners = new HashSet<>();
    private Snapshot snapshot = new Snapshot(false, false, false, "", "", Collections.emptyList());
    // Network state belongs to worker; snapshots and listeners belong to main.
    private String origin = "";
    private volatile String token = "";
    private long tokenDeadline;
    private volatile ScheduledFuture<?> polling;
    private volatile long lastSuccessfulSyncMs = -1;
    private final Set<String> notified = new HashSet<>();
    private volatile boolean destroyed;
    private volatile int generation;
    private volatile boolean foreground;

    private final Runnable healthCheck = new Runnable() {
        @Override public void run() {
            if (destroyed) return;
            ScheduledFuture<?> task = polling;
            if (!token.isEmpty() && task != null && task.isDone() && !task.isCancelled()) {
                // A fatal task failure must become a visible stop, never an apparently healthy duty loop.
                android.util.Log.e("ApprovalService", "Approval polling terminated unexpectedly");
                int expected = ++generation;
                token = ""; cancelPolling();
                publish(expected, false, false, false, "值守异常停止，请重新登录", Collections.emptyList());
                stopDuty(expected);
            } else if (snapshot.authenticated && snapshot.connected && !hasRecentSync()) {
                snapshot = new Snapshot(true, snapshot.busy, false, snapshot.origin,
                        "同步超时，暂不能审批；等待连接恢复", Collections.emptyList());
                for (Listener listener : new ArrayList<>(listeners)) listener.changed();
                notifySafely(DUTY_ID, DUTY, "手机授权同步超时", "无法确认申请状态，请打开 App 检查", true);
                cancelNotificationSafely(REQUEST_ID);
            }
            main.postDelayed(this, 1000);
        }
    };
    long lastSuccessfulSyncMs() { return lastSuccessfulSyncMs; }
    private boolean hasRecentSync() {
        return lastSuccessfulSyncMs >= 0 && SystemClock.elapsedRealtime() - lastSuccessfulSyncMs <= SYNC_TIMEOUT_MS;
    }
    // Keep platform/network boundaries replaceable for service lifecycle tests.
    JSONObject request(String path, String credential, JSONObject body) throws Exception {
        return ApprovalApi.request(origin, path, credential, body);
    }
    void deliverNotification(int id, Notification value) {
        getSystemService(NotificationManager.class).notify(id, value);
    }
    void cancelNotification(int id) { getSystemService(NotificationManager.class).cancel(id); }
    boolean notificationsEnabled() { return notificationsEnabled(REQUESTS); }
    boolean notificationsEnabled(String channelId) {
        try {
            if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) return false;
            NotificationManager manager = getSystemService(NotificationManager.class);
            NotificationChannel channel = manager.getNotificationChannel(channelId);
            return manager.areNotificationsEnabled() && channel != null && channel.getImportance() != NotificationManager.IMPORTANCE_NONE;
        } catch (RuntimeException error) {
            android.util.Log.w("ApprovalService", "Cannot read notification settings", error); return false;
        }
    }

    @Override public void onCreate() {
        super.onCreate();
        main.postDelayed(healthCheck, 1000);
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(DUTY, "值守状态", NotificationManager.IMPORTANCE_LOW));
        manager.createNotificationChannel(new NotificationChannel(REQUESTS, "车辆控制申请", NotificationManager.IMPORTANCE_HIGH));
    }
    @Override public IBinder onBind(Intent intent) { return new LocalBinder(); }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (!foreground) { startForeground(DUTY_ID, notification(DUTY, "手机授权值守", "正在连接云端…", true)); foreground = true; }
        return START_NOT_STICKY;
    }
    Snapshot snapshot() { return snapshot; }
    void addListener(Listener listener) { listeners.add(listener); listener.changed(); }
    void removeListener(Listener listener) { listeners.remove(listener); }
    private void publish(int expected, boolean auth, boolean busy, boolean connected, String message, List<ApprovalRequest> requests) {
        String currentOrigin = origin;
        main.post(() -> {
            if (destroyed || generation != expected) return;
            snapshot = new Snapshot(auth, busy, connected, currentOrigin, message, requests);
            for (Listener listener : new ArrayList<>(listeners)) listener.changed();
        });
    }
    private Notification notification(String channel, String title, String text, boolean ongoing) {
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, channel).setSmallIcon(com.mineteleop.approval.R.drawable.ic_notification)
                .setContentTitle(title).setContentText(text).setContentIntent(open).setOngoing(ongoing)
                .setAutoCancel(!ongoing).setVisibility(Notification.VISIBILITY_PRIVATE)
                .setCategory(Notification.CATEGORY_STATUS).build();
    }
    private boolean notifySafely(int id, String channel, String title, String text, boolean ongoing) {
        try {
            if (destroyed || !notificationsEnabled(channel)) return false;
            deliverNotification(id, notification(channel, title, text, ongoing));
            return true;
        } catch (RuntimeException error) {
            android.util.Log.w("ApprovalService", "Cannot post notification", error); return false;
        }
    }
    private void cancelNotificationSafely(int id) {
        try { cancelNotification(id); }
        catch (RuntimeException error) { android.util.Log.w("ApprovalService", "Cannot cancel notification", error); }
    }
    void login(String newOrigin, String password) {
        if (snapshot.busy || snapshot.authenticated) return;
        int expected = ++generation;
        snapshot = new Snapshot(false, true, false, newOrigin, "正在登录…", Collections.emptyList());
        for (Listener listener : new ArrayList<>(listeners)) listener.changed();
        // Start while the activity is visible; onStartCommand must immediately post its notification.
        startForegroundService(new Intent(this, ApprovalService.class));
        worker.execute(() -> {
            origin = newOrigin;
            try {
                JSONObject result = request("login", "", new JSONObject()
                        .put("password", password));
                if (destroyed || generation != expected) return;
                token = result.getString("token");
                long ttl = Math.max(0, Math.min(7L * 24 * 60 * 60 * 1000,
                        result.has("remaining_ms") ? result.getLong("remaining_ms")
                        : result.getLong("expires_at_utc_ms") - System.currentTimeMillis()));
                tokenDeadline = SystemClock.elapsedRealtime() + ttl;
                notified.clear(); lastSuccessfulSyncMs = -1;
                if (!destroyed && generation == expected)
                    polling = worker.scheduleWithFixedDelay(() -> poll(expected), 0, 3, TimeUnit.SECONDS);
            } catch (Exception error) {
                token = "";
                publish(expected, false, false, false, describe(error), Collections.emptyList());
                stopDuty(expected);
            }
        });
    }
    private void poll(int expected) {
        if (destroyed || generation != expected || token.isEmpty()) return;
        try {
            if (SystemClock.elapsedRealtime() >= tokenDeadline) throw new ApprovalApi.Failure(401, "审批登录已过期，请重新登录");
            JSONArray rows = request("requests", token, null).getJSONArray("requests");
            if (destroyed || generation != expected) return;
            List<ApprovalRequest> requests = new ArrayList<>();
            long now = SystemClock.elapsedRealtime();
            Set<String> pending = new HashSet<>();
            boolean hasNew = false;
            for (int i = 0; i < rows.length(); i++) {
                JSONObject row = rows.getJSONObject(i);
                ApprovalRequest request = new ApprovalRequest(row.getString("request_id"), row.getString("vehicle_id"),
                        row.getString("driver_id"), row.getString("state"), row.getLong("remaining_ms"), now);
                requests.add(request);
                if (request.actionable(now)) { pending.add(request.id); if (!notified.contains(request.id)) hasNew = true; }
            }
            notified.retainAll(pending);
            lastSuccessfulSyncMs = SystemClock.elapsedRealtime();
            if (hasNew && notifySafely(REQUEST_ID, REQUESTS, "有车辆等待授权", "请打开 App 核对车辆与申请人，再同意或拒绝", false))
                notified.addAll(pending);
            if (pending.isEmpty()) cancelNotificationSafely(REQUEST_ID);
            publish(expected, true, false, true, pending.isEmpty() ? "值守中 · 当前没有待确认申请" : "请核对车辆与申请人后再授权", requests);
            notifySafely(DUTY_ID, DUTY, "手机授权值守中", pending.isEmpty() ? "等待新的车辆控制申请" : pending.size() + " 条申请等待确认", true);
        } catch (Exception error) {
            if (destroyed || generation != expected) return;
            if (error instanceof ApprovalApi.Failure && ((ApprovalApi.Failure) error).status == 401) {
                token = ""; cancelPolling(); publish(expected, false, false, false, "审批登录已失效，请重新登录", Collections.emptyList()); stopDuty(expected);
            } else {
                publish(expected, true, false, false, "云端连接中断，暂不能审批；正在重试", Collections.emptyList());
                notifySafely(DUTY_ID, DUTY, "手机授权暂时离线", "无法接收申请，正在重试连接", true);
                cancelNotificationSafely(REQUEST_ID);
            }
        }
    }
    void decide(String requestId, String decision) {
        if (!snapshot.authenticated || !snapshot.connected || !hasRecentSync() || snapshot.busy ||
                !("approve".equals(decision) || "reject".equals(decision))) return;
        if (snapshot.requests.stream().noneMatch(r -> r.id.equals(requestId) && r.actionable(SystemClock.elapsedRealtime()))) return;
        int expected = generation;
        snapshot = new Snapshot(true, true, true, snapshot.origin, "正在提交决定…", snapshot.requests);
        for (Listener listener : new ArrayList<>(listeners)) listener.changed();
        worker.execute(() -> {
            try {
                // Request identifiers originate from the server, but never allow them to alter the endpoint path.
                String encoded = java.net.URLEncoder.encode(requestId, "UTF-8");
                request("requests/" + encoded + "/decision", token, new JSONObject().put("decision", decision));
                poll(expected);
            } catch (Exception error) {
                // A lost response is ambiguous; refresh server state instead of asserting success.
                poll(expected);
                main.post(() -> {
                    if (destroyed || generation != expected || !snapshot.authenticated) return;
                    snapshot = new Snapshot(true, false, snapshot.connected, snapshot.origin,
                            "提交未确认：" + describe(error) + "。请以列表最新状态为准", snapshot.requests);
                    for (Listener listener : new ArrayList<>(listeners)) listener.changed();
                });
            }
        });
    }
    void logout() {
        if (snapshot.busy) return;
        int expected = ++generation;
        snapshot = new Snapshot(false, true, false, snapshot.origin, "正在结束值守…", Collections.emptyList());
        for (Listener listener : new ArrayList<>(listeners)) listener.changed();
        worker.execute(() -> {
            cancelPolling();
            String message = "已退出值守";
            try { if (!token.isEmpty()) request("logout", token, new JSONObject()); }
            catch (Exception error) { message = "已停止本机值守；云端注销未确认，登录令牌将到期失效"; }
            token = ""; notified.clear(); publish(expected, false, false, false, message, Collections.emptyList()); stopDuty(expected);
        });
    }
    private static String describe(Exception error) {
        if (error instanceof ApprovalApi.Failure) return error.getMessage();
        if (error instanceof javax.net.ssl.SSLException) return "HTTPS 证书验证失败，请检查云端证书";
        if (error instanceof java.io.IOException) return "网络请求失败，请检查云端地址和网络";
        return "云端响应异常，请检查服务版本";
    }
    private void cancelPolling() { if (polling != null) { polling.cancel(false); polling = null; } }
    private void stopDuty(int expected) {
        main.post(() -> {
            if (generation != expected) return;
            stopForeground(STOP_FOREGROUND_REMOVE); foreground = false;
            cancelNotificationSafely(REQUEST_ID); stopSelf();
        });
    }
    @Override public void onDestroy() {
        destroyed = true; ++generation; token = ""; main.removeCallbacks(healthCheck); worker.shutdownNow(); listeners.clear();
        cancelNotificationSafely(REQUEST_ID);
        super.onDestroy();
    }
}
