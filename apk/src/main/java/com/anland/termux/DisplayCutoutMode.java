package com.anland.termux;

import android.content.SharedPreferences;

final class DisplayCutoutMode {
    static final String KEY = "hide_display_cutout";

    static final String HIDE_ALL = "hide_all";
    static final String HIDE_ROUNDED_CORNERS_ONLY = "rounded_corners_only";
    static final String HIDE_CUTOUT_ONLY = "cutout_only";
    static final String HIDE_NONE = "none";

    static final String[] VALUES = {
        HIDE_ALL, HIDE_ROUNDED_CORNERS_ONLY, HIDE_CUTOUT_ONLY, HIDE_NONE
    };

    private DisplayCutoutMode() {}

    static String get(SharedPreferences prefs) {
        Object stored = prefs.getAll().get(KEY);
        if (stored instanceof String && isValid((String) stored))
            return (String) stored;

        String mode = stored instanceof Boolean && !((Boolean) stored)
            ? HIDE_NONE
            : HIDE_ALL;
        if (stored != null)
            prefs.edit().putString(KEY, mode).apply();
        return mode;
    }

    static boolean hidesCutout(String mode) {
        return HIDE_ALL.equals(mode) || HIDE_CUTOUT_ONLY.equals(mode);
    }

    static boolean hidesRoundedCorners(String mode) {
        return HIDE_ALL.equals(mode) || HIDE_ROUNDED_CORNERS_ONLY.equals(mode);
    }

    private static boolean isValid(String mode) {
        for (String value : VALUES) {
            if (value.equals(mode))
                return true;
        }
        return false;
    }
}
