package andro.marchy;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Tells the Linux side whether a physical keyboard (USB or Bluetooth) is connected, like Android hides its soft
 * keyboard then. The Linux side cannot see input devices (no /dev/input for apps), so the app writes "1" or "0" to
 * run/hardware-keyboard; squeekboard-session turns Squeekboard's automatic display off while it is 1.
 */
final class HardwareKeyboard implements InputManager.InputDeviceListener {
    private final Context ctx;
    private final InputManager im;

    private HardwareKeyboard(Context ctx) {
        this.ctx = ctx.getApplicationContext();
        this.im = this.ctx.getSystemService(InputManager.class);
    }

    private static HardwareKeyboard instance; // once per process: the activity is recreated on rotation

    static void watch(Context ctx) {
        if (instance != null)
            return;
        HardwareKeyboard hk = instance = new HardwareKeyboard(ctx);
        hk.im.registerInputDeviceListener(hk, new Handler(Looper.getMainLooper()));
        hk.update();
    }

    private boolean connected() {
        for (int id : im.getInputDeviceIds()) {
            InputDevice d = im.getInputDevice(id);
            if (d != null && !d.isVirtual() && d.isExternal()
                    && d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC && !mouseButtons(d))
                return true;
        }
        return false;
    }

    /**
     * Many mice also present a keyboard for their extra buttons ("Jomaa BT2 Keyboard" next to "Jomaa BT2 Mouse",
     * same vendor and product): Android calls it a full keyboard, and the on-screen one stayed hidden. Not a keyboard
     * when its hardware has a part named "Mouse" (keyboards with a touchpad do not name theirs so).
     */
    private boolean mouseButtons(InputDevice kbd) {
        // the mouse itself also reports an alphabetic keyboard (its buttons): a mouse, whatever else it says
        if (kbd.getName().toLowerCase().contains("mouse") || kbd.supportsSource(InputDevice.SOURCE_MOUSE))
            return true;
        for (int id : im.getInputDeviceIds()) {
            InputDevice o = im.getInputDevice(id);
            if (o != null && o.getId() != kbd.getId() && o.getVendorId() == kbd.getVendorId()
                    && o.getProductId() == kbd.getProductId() && o.getName().toLowerCase().contains("mouse"))
                return true;
        }
        return false;
    }

    private void update() {
        File f = new File(SessionService.runDir(ctx), "hardware-keyboard");
        f.getParentFile().mkdirs();
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(connected() ? '1' : '0');
        } catch (IOException ignored) {
        }
    }

    @Override public void onInputDeviceAdded(int id) { update(); }
    @Override public void onInputDeviceRemoved(int id) { update(); }
    @Override public void onInputDeviceChanged(int id) { update(); }
}
