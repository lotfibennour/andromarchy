package andro.marchy;

import android.os.ParcelFileDescriptor;

interface ICompatibleBridge {
    ParcelFileDescriptor getConnection();
}
