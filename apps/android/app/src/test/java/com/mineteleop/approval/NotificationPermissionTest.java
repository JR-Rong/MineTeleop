package com.mineteleop.approval;

import android.Manifest;
import android.app.NotificationManager;
import android.content.Context;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.*;
import org.junit.*;
import org.junit.runner.RunWith;
import org.robolectric.*;
import org.robolectric.android.controller.*;
import org.robolectric.annotation.Config;
import org.robolectric.util.ReflectionHelpers;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 33)
public class NotificationPermissionTest {
    public static class PermissionService extends ApprovalService {
        int logins;
        @Override void login(String origin, String password) { ++logins; }
    }
    private ActivityController<MainActivity> activityController;
    private ServiceController<PermissionService> serviceController;
    private MainActivity activity;
    private PermissionService service;
    @Before public void setUp() {
        shadowOf(RuntimeEnvironment.getApplication()).denyPermissions(Manifest.permission.POST_NOTIFICATIONS);
        serviceController = Robolectric.buildService(PermissionService.class).create(); service = serviceController.get();
        activityController = Robolectric.buildActivity(MainActivity.class).create(); activity = activityController.get();
        ReflectionHelpers.setField(activity, "service", service); activity.changed();
    }
    @After public void tearDown() {
        activityController.destroy(); serviceController.destroy(); shadowOf(Looper.getMainLooper()).idle();
    }
    @Test public void oldDeniedOncePreferenceDoesNotBlockNewPermissionRequestOrManualLogin() {
        activity.getSharedPreferences("login_preferences", Context.MODE_PRIVATE).edit().putBoolean("notification_asked", true).commit();
        EditText password = ReflectionHelpers.getField(activity, "passwordInput"); password.setText("test-password");
        Button login = ReflectionHelpers.getField(activity, "loginButton"); login.performClick();
        assertNotNull(shadowOf(activity).getLastRequestedPermission());
        assertEquals(1, service.logins);
    }
    @Test public void settingsRemainReachableAndPermissionRecoveryHidesWarning() {
        NotificationManager manager = activity.getSystemService(NotificationManager.class);
        shadowOf(manager).setNotificationsEnabled(false); activity.changed();
        Button settings = ReflectionHelpers.getField(activity, "notificationButton");
        assertEquals(View.VISIBLE, settings.getVisibility()); settings.performClick();
        assertEquals(Settings.ACTION_APP_NOTIFICATION_SETTINGS, shadowOf(activity).getNextStartedActivity().getAction());
        shadowOf(RuntimeEnvironment.getApplication()).grantPermissions(Manifest.permission.POST_NOTIFICATIONS);
        shadowOf(manager).setNotificationsEnabled(true); activity.changed();
        assertTrue(service.notificationsEnabled()); assertEquals(View.GONE, settings.getVisibility());
    }
}
