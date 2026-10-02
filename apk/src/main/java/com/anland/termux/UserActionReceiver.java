package com.anland.termux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public class UserActionReceiver extends BroadcastReceiver {
    static final String EXTRA_RESPONSE = "response";

    @Override
    public void onReceive(Context context, Intent intent) {
        MainActivity activity = MainActivity.sInstance;
        if (activity != null && !activity.isFinishing())
            activity.performUserAction(intent.getStringExtra(EXTRA_RESPONSE));
    }
}
