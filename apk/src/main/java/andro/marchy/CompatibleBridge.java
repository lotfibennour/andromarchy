package andro.marchy;

import android.content.Context;
import android.content.Intent;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.IOException;
import java.lang.reflect.Method;

/**
 * Termux-side entry point for the APK without sharedUserId.
 *
 * The command installed by the Andromarchy Termux package starts this class with
 * app_process. It connects to the daemon as the Termux UID and publishes a
 * Binder which lets the Android app duplicate that connected socket fd.
 */
public final class CompatibleBridge {
    public static final String ACTION_START = "andro.marchy.COMPATIBLE_BRIDGE_START";

    private static final String TAG = "AndromarchyCompatibleBridge";
    private static final String APPLICATION_ID = BuildConfig.APPLICATION_ID;

    private final Context context;
    private final BridgeBinder binder;

    private CompatibleBridge(String socketPath) throws IOException {
        context = createContext();
        if (context == null)
            throw new IOException("could not create an Android system context");
        binder = new BridgeBinder(socketPath);
    }

    public static void main(String[] args) {
        if (Looper.getMainLooper() == null)
            Looper.prepareMainLooper();

        String socketPath = args.length == 1 ? args[0] : null;
        if (socketPath == null || socketPath.isEmpty()) {
            System.err.println("usage: CompatibleBridge SOCKET_PATH");
            return;
        }

        try {
            new CompatibleBridge(socketPath).run();
        } catch (Exception e) {
            Log.e(TAG, "compatible bridge failed", e);
            e.printStackTrace(System.err);
        }
    }

    private void run() {
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable broadcaster = new Runnable() {
            @Override
            public void run() {
                // Keep publishing the Binder so a recreated Activity can attach to
                // the already-running Termux-side bridge without starting another
                // process. MainActivity ignores duplicate broadcasts while its fd
                // is still in use.
                sendBroadcast();
                handler.postDelayed(this, 1000L);
            }
        };
        handler.post(broadcaster);
        Looper.loop();
    }

    private void sendBroadcast() {
        Bundle bundle = new Bundle();
        bundle.putBinder(null, binder);

        Intent intent = new Intent(ACTION_START);
        intent.putExtra(null, bundle);
        intent.setPackage(APPLICATION_ID);
        try {
            context.sendBroadcast(intent);
        } catch (RuntimeException e) {
            Log.e(TAG, "failed to send compatible bridge broadcast", e);
        }
    }

    private static Context createContext() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field unsafeField = unsafeClass.getDeclaredField("theUnsafe");
            unsafeField.setAccessible(true);
            Object unsafe = unsafeField.get(null);
            Method allocateInstance = unsafeClass.getMethod("allocateInstance", Class.class);
            Object activityThread = allocateInstance.invoke(unsafe, activityThreadClass);
            Method getSystemContext = activityThreadClass.getDeclaredMethod("getSystemContext");
            getSystemContext.setAccessible(true);
            return (Context) getSystemContext.invoke(activityThread);
        } catch (Exception e) {
            Log.e(TAG, "failed to create Android system context", e);
            return null;
        }
    }

    private static final class BridgeBinder extends ICompatibleBridge.Stub {
        private final String socketPath;
        private LocalSocket socket;

        BridgeBinder(String socketPath) {
            this.socketPath = socketPath;
        }

        @Override
        public synchronized ParcelFileDescriptor getConnection() {
            try {
                if (socket == null) {
                    LocalSocket candidate = new LocalSocket();
                    try {
                        candidate.connect(new LocalSocketAddress(socketPath,
                            LocalSocketAddress.Namespace.FILESYSTEM));
                        socket = candidate;
                    } catch (IOException e) {
                        candidate.close();
                        Log.e(TAG, "failed to connect to daemon socket " + socketPath, e);
                        return null;
                    }
                }
                return ParcelFileDescriptor.dup(socket.getFileDescriptor());
            } catch (IOException e) {
                closeSocket();
                Log.e(TAG, "failed to duplicate daemon socket fd", e);
                return null;
            }
        }

        private void closeSocket() {
            if (socket == null)
                return;
            try {
                socket.close();
            } catch (IOException ignored) {
            }
            socket = null;
        }
    }
}
