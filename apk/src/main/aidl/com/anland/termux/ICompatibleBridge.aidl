package com.anland.termux;

import android.os.ParcelFileDescriptor;

interface ICompatibleBridge {
    ParcelFileDescriptor getConnection();
}
