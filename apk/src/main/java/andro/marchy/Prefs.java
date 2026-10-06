package andro.marchy;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.Map;

/**
 * App settings defaults, and their move from the former file name (anland_settings). Called first thing by the
 * activity and the accessibility service; idempotent.
 */
final class Prefs {
    static final String NAME = "andromarchy_settings";
    private static final String OLD_NAME = "anland_settings";

    // The validated setup (tablet, 2026-10-06): what a new install starts with.
    private static final Object[][] DEFAULTS = {
        {"screen_orientation", "auto"},
        {"hide_display_cutout", "hide_all"},
        {"transform_captured_pointer", "no"},
        {"accessibility_key_intercept", true},
        {"camera_enabled", true},
        {"mic_enabled", true},
        {"speaker_latency_ms", 0},
        {"mic_latency_ms", 0},
        {"back_button_action", "toggle additional key bar"},
        {"volume_up_action", "no action"},
        {"volume_down_action", "no action"},
        {"media_keys_action", "no action"},
        {"notification_tap_action", "open preferences"},
        {"notification_first_button_action", "open preferences"},
        {"notification_second_button_action", "exit"},
    };

    private static boolean done;

    static synchronized void init(Context ctx) {
        if (done)
            return;
        done = true;
        SharedPreferences p = ctx.getSharedPreferences(NAME, Context.MODE_PRIVATE);
        SharedPreferences.Editor e = p.edit();
        SharedPreferences old = ctx.getSharedPreferences(OLD_NAME, Context.MODE_PRIVATE);
        for (Map.Entry<String, ?> kv : old.getAll().entrySet())
            if (!p.contains(kv.getKey()))
                put(e, kv.getKey(), kv.getValue());
        for (Object[] d : DEFAULTS)
            if (!p.contains((String) d[0]) && !old.contains((String) d[0]))
                put(e, (String) d[0], d[1]);
        e.commit();
        if (!old.getAll().isEmpty())
            ctx.deleteSharedPreferences(OLD_NAME);
    }

    private static void put(SharedPreferences.Editor e, String k, Object v) {
        if (v instanceof Boolean) e.putBoolean(k, (Boolean) v);
        else if (v instanceof Integer) e.putInt(k, (Integer) v);
        else if (v instanceof Long) e.putLong(k, (Long) v);
        else if (v instanceof Float) e.putFloat(k, (Float) v);
        else if (v != null) e.putString(k, v.toString());
    }
}
