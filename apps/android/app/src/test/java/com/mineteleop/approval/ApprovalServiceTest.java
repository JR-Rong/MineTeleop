package com.mineteleop.approval;

import android.app.Notification;
import android.content.Intent;
import android.os.Looper;
import org.json.*;
import org.junit.*;
import org.junit.runner.RunWith;
import org.robolectric.*;
import org.robolectric.android.controller.ServiceController;
import org.robolectric.annotation.Config;
import java.io.IOException;
import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 33)
public class ApprovalServiceTest {
    public static class TestService extends ApprovalService {
        volatile boolean offline, brokenNotifications, fatal, allowed = true;
        volatile CountDownLatch holdRequests;
        final AtomicInteger polls = new AtomicInteger(), reminders = new AtomicInteger(), decisions = new AtomicInteger();
        @Override boolean notificationsEnabled(String channelId) { return allowed; }
        @Override void deliverNotification(int id, Notification notification) {
            if (brokenNotifications) throw new SecurityException("notification fixture failure");
            if (id == 2) reminders.incrementAndGet();
        }
        @Override void cancelNotification(int id) {
            if (brokenNotifications) throw new IllegalStateException("cancel fixture failure");
        }
        @Override JSONObject request(String path, String credential, JSONObject body) throws Exception {
            if (path.equals("login"))
                // Relative TTL must work even when the device wall clock disagrees with the server.
                return new JSONObject().put("token", "test-token").put("remaining_ms", 60000).put("expires_at_utc_ms", 0);
            if (path.equals("logout")) return new JSONObject();
            if (path.endsWith("/decision")) { decisions.incrementAndGet(); return new JSONObject(); }
            polls.incrementAndGet();
            CountDownLatch latch = holdRequests;
            if (latch != null && !latch.await(8, TimeUnit.SECONDS)) throw new IOException("fixture stalled");
            if (fatal) throw new AssertionError("fatal scheduled task fixture");
            if (offline) throw new IOException("offline fixture");
            return new JSONObject().put("requests", new JSONArray().put(new JSONObject()
                    .put("request_id", "r1").put("vehicle_id", "v1").put("driver_id", "d1")
                    .put("state", "pending").put("remaining_ms", 60000)));
        }
    }
    private ServiceController<TestService> controller;
    private TestService service;
    @Before public void setUp() {
        controller = Robolectric.buildService(TestService.class).create(); service = controller.get();
        service.onStartCommand(new Intent(), 0, 1);
    }
    @After public void tearDown() {
        if (service.holdRequests != null) service.holdRequests.countDown();
        controller.destroy(); shadowOf(Looper.getMainLooper()).idle();
    }
    private void await(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        do {
            shadowOf(Looper.getMainLooper()).idle();
            if (condition.getAsBoolean()) return;
            Thread.sleep(10);
        } while (System.nanoTime() < deadline);
        fail("service did not reach expected state: " + service.snapshot().message);
    }
    private void login() throws Exception {
        service.login("https://example.com", "test-password");
        await(() -> service.snapshot().authenticated && service.snapshot().connected);
    }
    @Test public void loginUsesRelativeTtlAndOnlyNotifiesNewRequestOnce() throws Exception {
        login(); await(() -> service.polls.get() >= 2);
        assertEquals(1, service.reminders.get()); assertTrue(service.snapshot().authenticated);
    }
    @Test public void networkAndNotificationFailuresDoNotKillScheduledPolling() throws Exception {
        login(); service.offline = true; service.brokenNotifications = true;
        await(() -> !service.snapshot().connected);
        assertTrue(service.snapshot().authenticated);
        service.offline = false;
        await(() -> service.snapshot().connected);
        assertTrue(service.polls.get() >= 3);
    }
    @Test public void enablingNotificationsRemindsForAnAlreadyPendingRequest() throws Exception {
        service.allowed = false; login(); assertEquals(0, service.reminders.get());
        service.allowed = true; await(() -> service.reminders.get() == 1);
    }
    @Test public void expiredTokenStopsDuty() throws Exception {
        login(); shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(61));
        await(() -> !service.snapshot().authenticated);
        assertTrue(service.snapshot().message.contains("失效"));
        assertTrue(shadowOf(service).isForegroundStopped());
    }
    @Test public void failureOfFirstPollDoesNotLeaveLoginBusyForever() throws Exception {
        service.fatal = true; service.login("https://example.com", "test-password");
        await(() -> service.polls.get() >= 1);
        await(() -> {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));
            return !service.snapshot().busy;
        });
        assertFalse(service.snapshot().authenticated);
        assertTrue(service.snapshot().message.contains("异常停止"));
    }
    @Test public void fatalScheduledFailureBecomesVisibleStop() throws Exception {
        login(); service.fatal = true;
        await(() -> service.polls.get() >= 2);
        // Wait for the scheduled Future to complete exceptionally, then run the independent health check.
        await(() -> {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));
            return !service.snapshot().authenticated;
        });
        assertTrue(service.snapshot().message.contains("异常停止"));
        assertTrue(shadowOf(service).isForegroundStopped());
    }
    @Test public void stalledSyncDisablesDecisionsUntilRecovery() throws Exception {
        login(); shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(31));
        assertFalse(service.snapshot().connected);
        service.decide("r1", "approve"); assertEquals(0, service.decisions.get());
        await(() -> service.snapshot().connected);
    }
    @Test public void latePollCannotRestoreLoggedOutSession() throws Exception {
        login(); service.holdRequests = new CountDownLatch(1);
        await(() -> service.polls.get() >= 2);
        service.logout(); service.holdRequests.countDown();
        await(() -> !service.snapshot().authenticated && !service.snapshot().busy);
        assertTrue(service.snapshot().requests.isEmpty());
        assertTrue(shadowOf(service).isForegroundStopped());
    }
}
