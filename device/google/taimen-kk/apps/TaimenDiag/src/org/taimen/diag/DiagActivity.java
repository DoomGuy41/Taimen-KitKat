package org.taimen.diag;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.FeatureInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.Camera;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.location.GpsSatellite;
import android.location.GpsStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.media.RingtoneManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.nfc.NfcAdapter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.StatFs;
import android.os.SystemClock;
import android.os.Vibrator;
import android.provider.Settings;
import android.telephony.PhoneStateListener;
import android.telephony.ServiceState;
import android.telephony.SignalStrength;
import android.telephony.TelephonyManager;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TimeZone;

/**
 * Taimen Diag: one text page per section, test buttons above it.
 * "Live" sections are refreshed every second.
 */
public class DiagActivity extends Activity {
    private final Handler h = new Handler();
    private LinearLayout actions;
    private TextView body;
    private Section current;
    private final List<Section> sections = new ArrayList<Section>();
    private final List<String> log = new ArrayList<String>();

    // ------------------------------------------------------------------ sections
    private abstract class Section {
        final String name;
        final boolean live;
        Section(String name, boolean live) { this.name = name; this.live = live; sections.add(this); }
        void enter() {}
        void leave() {}
        void buttons() {}
        abstract void render(StringBuilder sb);
    }

    private void button(String label, final Runnable r) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(12);
        b.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                try { r.run(); } catch (Throwable t) { note("error: " + t); }
                refresh();
            }
        });
        actions.addView(b, new LinearLayout.LayoutParams(-2, -2));
    }

    /** Result message of a test, shown at the top of the section. */
    void note(final String s) {
        h.post(new Runnable() {
            public void run() {
                log.add(0, new SimpleDateFormat("HH:mm:ss").format(new Date()) + "  " + s);
                while (log.size() > 12) log.remove(log.size() - 1);
                refresh();
            }
        });
    }

    private void background(final Runnable r) {
        new Thread(new Runnable() {
            public void run() {
                try { r.run(); } catch (Throwable t) { note("error: " + t); }
            }
        }).start();
    }

    // ------------------------------------------------------------------ interface
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        HorizontalScrollView tabsScroll = new HorizontalScrollView(this);
        LinearLayout tabs = new LinearLayout(this);
        tabsScroll.addView(tabs);
        root.addView(tabsScroll, new LinearLayout.LayoutParams(-1, -2));

        HorizontalScrollView actScroll = new HorizontalScrollView(this);
        actions = new LinearLayout(this);
        actScroll.addView(actions);
        root.addView(actScroll, new LinearLayout.LayoutParams(-1, -2));

        ScrollView sv = new ScrollView(this);
        body = new TextView(this);
        body.setTypeface(Typeface.MONOSPACE);
        body.setTextSize(11);
        body.setTextColor(0xffe0e0e0);
        body.setPadding(12, 8, 12, 48);
        body.setTextIsSelectable(true);
        sv.addView(body);
        root.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));
        setContentView(root);

        createSections();
        for (final Section s : sections) {
            Button t = new Button(this);
            t.setText(s.name);
            t.setAllCaps(false);
            t.setTextSize(12);
            t.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { show(s); }
            });
            tabs.addView(t, new LinearLayout.LayoutParams(-2, -2));
        }
        Section first = sections.get(0);
        String want = getIntent().getStringExtra("section");  // am start ... -e section GPU
        for (Section x : sections) if (x.name.equals(want)) first = x;
        show(first);
    }

    private void show(Section s) {
        if (current != null) current.leave();
        current = s;
        log.clear();
        actions.removeAllViews();
        s.buttons();
        s.enter();
        refresh();
    }

    void refresh() {
        if (current == null) return;
        StringBuilder sb = new StringBuilder();
        sb.append("=== ").append(current.name).append(current.live ? "   (live)" : "").append(" ===\n");
        for (String l : log) sb.append("> ").append(l).append('\n');
        if (!log.isEmpty()) sb.append('\n');
        try { current.render(sb); } catch (Throwable t) { sb.append("\ndisplay error: ").append(t); }
        body.setText(sb);
        // text copy for adb: /data/data/org.taimen.diag/files/<section>.txt
        Util.write(new File(getFilesDir(), current.name.replace('/', '_') + ".txt").getPath(), sb.toString());
    }

    private final Runnable ticker = new Runnable() {
        public void run() {
            if (current != null && current.live) refresh();
            h.postDelayed(this, 1000);
        }
    };

    @Override protected void onResume() { super.onResume(); h.post(ticker); if (current != null) current.enter(); }
    @Override protected void onPause() { super.onPause(); h.removeCallbacks(ticker); if (current != null) current.leave(); }

    // keys: shown in the Touch/keys section
    private String lastKey = "none";
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN)
            lastKey = KeyEvent.keyCodeToString(e.getKeyCode()) + " (code " + e.getKeyCode() + ", scan " + e.getScanCode() + ")";
        if (current != null && current.name.startsWith("Touch") && e.getKeyCode() != KeyEvent.KEYCODE_BACK) {
            refresh();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    // ------------------------------------------------------------------ shared data
    private SensorManager sm;
    private final Map<Integer, float[]> sensorValues = new HashMap<Integer, float[]>();
    private final Map<Integer, Long> sensorCounts = new HashMap<Integer, Long>();
    private final SensorEventListener sensorListener = new SensorEventListener() {
        public void onSensorChanged(SensorEvent e) {
            synchronized (sensorValues) {
                int k = System.identityHashCode(e.sensor);
                sensorValues.put(k, e.values.clone());
                Long c = sensorCounts.get(k);
                sensorCounts.put(k, c == null ? 1 : c + 1);
            }
        }
        public void onAccuracyChanged(Sensor s, int a) {}
    };

    private void sensorsOn() {
        for (Sensor s : sm.getSensorList(Sensor.TYPE_ALL)) {
            if (s.getType() == Sensor.TYPE_SIGNIFICANT_MOTION) continue;  // one-shot sensor
            sm.registerListener(sensorListener, s, SensorManager.SENSOR_DELAY_UI);
        }
    }

    private void sensorsOff() { sm.unregisterListener(sensorListener); }

    private static final String[] SENSOR_TYPES = { "?", "accelerometer", "magnetic field", "orientation",
        "gyroscope", "light", "pressure", "temperature", "proximity", "gravity", "linear accel.",
        "rotation vector", "humidity", "ambient temp.", "uncalibrated magnetometer", "game rotation",
        "uncalibrated gyro", "significant motion", "step detector", "step counter", "geomagnetic rotation" };

    private static String sensorType(int t) {
        return t >= 0 && t < SENSOR_TYPES.length ? SENSOR_TYPES[t] : "type " + t;
    }

    // ------------------------------------------------------------------ all sections
    private void createSections() {
        sm = (SensorManager) getSystemService(SENSOR_SERVICE);
        final AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        final TelephonyManager tm = (TelephonyManager) getSystemService(TELEPHONY_SERVICE);
        final WifiManager wm = (WifiManager) getSystemService(WIFI_SERVICE);
        final ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        final LocationManager lm = (LocationManager) getSystemService(LOCATION_SERVICE);
        final Vibrator vib = (Vibrator) getSystemService(VIBRATOR_SERVICE);
        final NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);

        // ============================================================== Summary
        new Section("Summary", true) {
            final Map<String, String> res = new java.util.LinkedHashMap<String, String>();
            boolean running;

            void put(String k, String v) { synchronized (res) { res.put(k, v); } }

            void buttons() { button("Run again", new Runnable() { public void run() { start(); } }); }

            void enter() { if (res.isEmpty()) start(); }

            void leave() { sensorsOff(); }

            void start() {
                if (running) return;
                running = true;
                synchronized (res) { res.clear(); }
                synchronized (sensorValues) { sensorValues.clear(); sensorCounts.clear(); }
                sensorsOn();
                scheduleAlarm(5000, "summary");
                put("Alarm (wake-up, 5 s)", "... waiting");
                background(new Runnable() {
                    public void run() {
                        GpuInfo.probe();
                        put("GPU ES 1 context", GpuInfo.es1Ok ? "OK" : "FAIL");
                        put("GPU ES 2 context + draw", GpuInfo.es2Ok && GpuInfo.renderOk ? "OK" : "FAIL");
                        put("GPU ES 3 context + GLSL 3.00", GpuInfo.es3Ok && GpuInfo.es3ShaderOk ? "OK" : "FAIL");
                        put("GPU renderer", GpuInfo.renderer);
                        put("Microphone (1 s)", micTest(1000, false));
                    }
                });
                h.postDelayed(new Runnable() { public void run() { finish1(); } }, 6000);
            }

            void finish1() {
                // sensors: at least one event within 6 s (trigger sensors excluded)
                List<Sensor> list = sm.getSensorList(Sensor.TYPE_ALL);
                int silent = 0;
                StringBuilder s = new StringBuilder();
                synchronized (sensorValues) {
                    for (Sensor x : list) {
                        int t = x.getType();
                        if (t == Sensor.TYPE_SIGNIFICANT_MOTION || t == Sensor.TYPE_STEP_DETECTOR
                                || t == Sensor.TYPE_STEP_COUNTER || t == Sensor.TYPE_PROXIMITY || t == Sensor.TYPE_LIGHT) continue;
                        if (!sensorCounts.containsKey(System.identityHashCode(x))) { silent++; s.append(' ').append(x.getName()); }
                    }
                }
                put("Sensors", list.size() + " declared, " + (silent == 0 ? "all report: OK" : silent + " silent:" + s));
                sensorsOff();
                running = false;
                refresh();
            }

            void render(StringBuilder sb) {
                sb.append("Automatic checks. OK = works, FAIL = broken, ? = check by hand.\n\n");
                // immediate checks
                Intent bat = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                int lvl = bat == null ? -1 : bat.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                line(sb, "Battery", lvl >= 0 ? "OK " + lvl + " %, " + bat.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0 + " °C" : "FAIL");
                String tz = Util.read("/sys/class/thermal/thermal_zone0/temp");
                line(sb, "CPU temperature (zone0)", Util.ok(tz) ? "OK " + Util.temp(tz) : "FAIL " + tz);
                String bl = Util.read("/sys/class/leds/lcd-backlight/brightness");
                line(sb, "Backlight (lights HAL)", new File("/system/lib/hw/lights.taimen.so").exists() && Util.ok(bl) ? "OK (value " + bl + ")" : "FAIL");
                int year = Integer.parseInt(new SimpleDateFormat("yyyy").format(new Date()));
                line(sb, "Clock", year >= 2024 ? "OK " + new Date() : "FAIL date " + new Date());
                line(sb, "Automatic time", Settings.Global.getInt(getContentResolver(), Settings.Global.AUTO_TIME, 0) == 1 ? "on" : "off");
                line(sb, "Alarm (wake-up, 5 s)", AlarmReceiver.ok == null ? AlarmReceiver.result : AlarmReceiver.result);
                line(sb, "Wi-Fi", wm.isWifiEnabled() ? (wm.getConnectionInfo().getIpAddress() != 0 ? "OK connected " + wm.getConnectionInfo().getSSID() : "on, not connected") : "off (?)");
                BluetoothAdapter ba = BluetoothAdapter.getDefaultAdapter();
                line(sb, "Bluetooth", ba == null ? "FAIL no adapter" : ba.isEnabled() ? "OK on " + ba.getAddress() : "off (?)");
                line(sb, "SIM", simState(tm.getSimState()));
                line(sb, "Mobile network", tm.getNetworkOperatorName().length() > 0 ? "OK " + tm.getNetworkOperatorName() + " " + netType(tm.getNetworkType()) : "FAIL not registered");
                line(sb, "IMEI", tm.getDeviceId() != null ? "OK " + tm.getDeviceId() : "FAIL");
                NetworkInfo mob = cm.getNetworkInfo(ConnectivityManager.TYPE_MOBILE);
                line(sb, "Mobile data", mob != null && mob.isConnected() ? "OK" : "FAIL (not connected)");
                line(sb, "Cameras", Camera.getNumberOfCameras() > 0 ? "OK " + Camera.getNumberOfCameras() : "FAIL none (no camera HAL)");
                line(sb, "Flash (sysfs)", new File("/sys/class/leds/led:torch_0/brightness").canWrite() ? "OK writable (test in Camera/Flash)" : "FAIL not accessible");
                line(sb, "LED notif (sysfs)", new File("/sys/class/leds/red/brightness").canWrite() ? "OK writable (test in LED/Vibrator)" : "FAIL not accessible");
                line(sb, "Vibrator", vib.hasVibrator() ? "OK present (test it)" : "FAIL");
                line(sb, "Sound output", AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT) > 0 ? "OK (test in Sound)" : "FAIL");
                line(sb, "GPS", lm.getAllProviders().contains(LocationManager.GPS_PROVIDER) ? "provider present" : "FAIL no GPS provider");
                line(sb, "NFC", NfcAdapter.getDefaultAdapter(DiagActivity.this) != null ? "OK" : "FAIL missing");
                line(sb, "/data", new File("/data/system").canWrite() ? "OK mounted read-write" : "FAIL");
                line(sb, "Boot slot", slot());
                synchronized (res) { for (Map.Entry<String, String> e : res.entrySet()) line(sb, e.getKey(), e.getValue()); }
                if (running) sb.append("\n... tests running (6 s)\n");
            }

            void line(StringBuilder sb, String k, String v) { sb.append(Util.pad(k, 30)).append(v).append('\n'); }
        };

        // ============================================================== System
        new Section("System", false) {
            void render(StringBuilder sb) {
                sb.append("model         ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE).append(", ").append(Build.PRODUCT).append(")\n");
                sb.append("hardware      ").append(Build.HARDWARE).append(", board ").append(Build.BOARD).append('\n');
                sb.append("Android       ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
                sb.append("build         ").append(Build.DISPLAY).append('\n');
                sb.append("fingerprint   ").append(Build.FINGERPRINT).append('\n');
                sb.append("type          ").append(Build.TYPE).append(' ').append(Build.TAGS).append('\n');
                sb.append("date build    ").append(new Date(Build.TIME)).append('\n');
                sb.append("kernel        ").append(Util.read("/proc/version")).append('\n');
                sb.append("baseband      ").append(Util.prop("gsm.version.baseband")).append('\n');
                sb.append("bootloader    ").append(Build.BOOTLOADER).append(", slot ").append(slot()).append('\n');
                sb.append("serial number ").append(Build.SERIAL).append('\n');
                sb.append("Java VM       ").append(System.getProperty("java.vm.name")).append(' ').append(System.getProperty("java.vm.version")).append(" (").append(Util.prop("persist.sys.dalvik.vm.lib")).append(")\n");
                sb.append("ABI           ").append(Build.CPU_ABI).append(' ').append(Build.CPU_ABI2).append('\n');
                sb.append("up for        ").append(SystemClock.elapsedRealtime() / 60000).append(" min\n");
                sb.append("\n== kernel command line\n").append(Util.read("/proc/cmdline")).append('\n');
                sb.append("\n== selected properties\n");
                String[] keys = { "ro.hardware", "ro.board.platform", "ro.opengles.version", "ro.sf.lcd_density",
                    "ro.build.characteristics", "ro.zygote.disable_gl_preload", "ro.kernel.qemu.gles", "wifi.interface",
                    "rild.libpath", "rild.libargs", "gsm.version.ril-impl", "ro.telephony.default_network",
                    "persist.sys.timezone", "persist.sys.language", "persist.sys.country", "ro.crypto.state",
                    "sys.usb.config", "sys.usb.state", "init.svc.fbpresentd", "init.svc.v11-sensors",
                    "init.svc.ril-daemon", "init.svc.wpa_supplicant", "init.svc.bluetoothd", "sys.v11.ready",
                    "ro.boot.bootdevice", "ro.serialno" };
                for (String k : keys) sb.append(Util.pad(k, 30)).append(Util.prop(k)).append('\n');
            }
        };

        // ============================================================== CPU / memory
        new Section("CPU/Mem", true) {
            void render(StringBuilder sb) {
                sb.append("load          ").append(Util.read("/proc/loadavg")).append('\n');
                for (String l : Util.read("/proc/cpuinfo").split("\n"))
                    if (l.startsWith("Hardware") || l.startsWith("Processor") || l.startsWith("model name")) { sb.append(l).append('\n'); break; }
                sb.append("cores         ").append(Runtime.getRuntime().availableProcessors()).append('\n');
                for (int i = 0; i < 8; i++) {
                    String d = "/sys/devices/system/cpu/cpu" + i;
                    if (!new File(d).exists()) break;
                    String on = new File(d + "/online").exists() ? Util.read(d + "/online") : "1";
                    sb.append("cpu").append(i).append(" online=").append(on);
                    sb.append("  freq ").append(mhz(Util.read(d + "/cpufreq/scaling_cur_freq")));
                    sb.append(" (").append(mhz(Util.read(d + "/cpufreq/cpuinfo_min_freq"))).append(" - ").append(mhz(Util.read(d + "/cpufreq/cpuinfo_max_freq"))).append(")");
                    sb.append("  governor ").append(Util.read(d + "/cpufreq/scaling_governor")).append('\n');
                }
                sb.append("\n== GPU (kgsl)\n");
                String g = "/sys/class/kgsl/kgsl-3d0/";
                sb.append("frequency     ").append(Util.read(g + "gpuclk")).append(" Hz (max ").append(Util.read(g + "max_gpuclk")).append(")\n");
                sb.append("busy          ").append(Util.read(g + "gpu_busy_percentage")).append('\n');
                sb.append("model         ").append(Util.read(g + "gpu_model")).append('\n');
                sb.append("\n== memory\n");
                ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
                ActivityManager a = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
                a.getMemoryInfo(mi);
                sb.append("available     ").append(Util.bytes(mi.availMem)).append(" / ").append(Util.bytes(mi.totalMem)).append(mi.lowMemory ? "  (low memory!)" : "").append('\n');
                sb.append("memory class  ").append(a.getMemoryClass()).append(" MiB (large ").append(a.getLargeMemoryClass()).append(")\n");
                String[] mem = Util.read("/proc/meminfo").split("\n");
                for (int i = 0; i < mem.length && i < 16; i++) sb.append(mem[i]).append('\n');
            }

            String mhz(String khz) {
                try { return Long.parseLong(khz.trim()) / 1000 + " MHz"; } catch (Exception e) { return khz; }
            }
        };

        // ============================================================== Temperatures
        new Section("Temperatures", true) {
            void render(StringBuilder sb) {
                sb.append("== thermal zones (/sys/class/thermal)\n");
                for (String z : Util.listNatural("/sys/class/thermal")) {
                    if (!z.startsWith("thermal_zone")) continue;
                    String d = "/sys/class/thermal/" + z;
                    String t = Util.read(d + "/temp");
                    sb.append(Util.pad(z, 15)).append(Util.pad(Util.read(d + "/type"), 20)).append(Util.ok(t) ? Util.temp(t) + "  (raw " + t + ")" : t).append('\n');
                }
                sb.append("\n== power supplies\n");
                for (String p : Util.list("/sys/class/power_supply")) {
                    String t = "/sys/class/power_supply/" + p + "/temp";
                    if (new File(t).exists()) sb.append(Util.pad(p, 15)).append(Util.temp(Util.read(t))).append('\n');
                }
            }
        };

        // ============================================================== Battery
        new Section("Battery", true) {
            void render(StringBuilder sb) {
                Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                if (b != null) {
                    sb.append("level         ").append(b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)).append(" / ").append(b.getIntExtra(BatteryManager.EXTRA_SCALE, -1)).append('\n');
                    String[] st = { "?", "unknown", "charging", "discharging", "not charging", "full" };
                    int s = b.getIntExtra(BatteryManager.EXTRA_STATUS, 0);
                    sb.append("status        ").append(s < st.length ? st[s] : "" + s).append('\n');
                    String[] he = { "?", "unknown", "good", "overheat", "dead", "over voltage", "failure", "cold" };
                    int hh = b.getIntExtra(BatteryManager.EXTRA_HEALTH, 0);
                    sb.append("health        ").append(hh < he.length ? he[hh] : "" + hh).append('\n');
                    int p = b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
                    sb.append("plugged       ").append(p == 0 ? "no" : (p & 1) != 0 ? "AC" : (p & 2) != 0 ? "USB" : "wireless").append('\n');
                    sb.append("voltage       ").append(b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0)).append(" mV\n");
                    sb.append("temperature   ").append(b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0).append(" °C\n");
                    sb.append("technology    ").append(b.getStringExtra(BatteryManager.EXTRA_TECHNOLOGY)).append('\n');
                }
                sb.append("\n== /sys/class/power_supply\n");
                for (String p : Util.list("/sys/class/power_supply")) {
                    String d = "/sys/class/power_supply/" + p + "/";
                    sb.append("[").append(p).append("]");
                    String[] keys = { "online", "present", "status", "capacity", "voltage_now", "current_now", "charge_full", "charge_full_design", "cycle_count", "type", "usb_type" };
                    for (String k : keys) {
                        if (!new File(d + k).exists()) continue;
                        sb.append(' ').append(k).append('=').append(Util.read(d + k));
                    }
                    sb.append('\n');
                }
            }
        };

        // ============================================================== Display
        new Section("Display", true) {
            void buttons() {
                int[] lv = { 5, 25, 50, 100 };
                for (final int p : lv) button(p + " %", new Runnable() { public void run() { setBrightness(-1, p * 255 / 100); } });
                button("Auto ON", new Runnable() { public void run() { setBrightness(1, -1); } });
                button("Auto OFF", new Runnable() { public void run() { setBrightness(0, -1); } });
            }

            void setBrightness(int mode, int value) {
                if (mode >= 0) Settings.System.putInt(getContentResolver(), Settings.System.SCREEN_BRIGHTNESS_MODE, mode);
                if (value >= 0) {
                    Settings.System.putInt(getContentResolver(), Settings.System.SCREEN_BRIGHTNESS_MODE, 0);
                    Settings.System.putInt(getContentResolver(), Settings.System.SCREEN_BRIGHTNESS, value);
                }
                h.postDelayed(new Runnable() {
                    public void run() { note("setting applied, sysfs backlight = " + Util.read("/sys/class/leds/lcd-backlight/brightness")); }
                }, 700);
            }

            void enter() {
                Sensor l = sm.getDefaultSensor(Sensor.TYPE_LIGHT);
                if (l != null) sm.registerListener(sensorListener, l, SensorManager.SENSOR_DELAY_UI);
            }

            void leave() { sensorsOff(); }

            void render(StringBuilder sb) {
                DisplayMetrics m = new DisplayMetrics();
                getWindowManager().getDefaultDisplay().getRealMetrics(m);
                sb.append("resolution    ").append(m.widthPixels).append(" x ").append(m.heightPixels).append('\n');
                sb.append("density       ").append(m.densityDpi).append(" dpi (x").append(m.density).append("), real ").append(m.xdpi).append(" x ").append(m.ydpi).append('\n');
                sb.append("refresh rate  ").append(getWindowManager().getDefaultDisplay().getRefreshRate()).append(" Hz\n");
                sb.append("rotation      ").append(getWindowManager().getDefaultDisplay().getRotation() * 90).append("°\n");
                int mode = Settings.System.getInt(getContentResolver(), Settings.System.SCREEN_BRIGHTNESS_MODE, -1);
                sb.append("brightness    setting ").append(Settings.System.getInt(getContentResolver(), Settings.System.SCREEN_BRIGHTNESS, -1))
                  .append("/255, mode ").append(mode == 1 ? "auto" : "manual").append('\n');
                sb.append("backlight     sysfs ").append(Util.read("/sys/class/leds/lcd-backlight/brightness")).append(" / ").append(Util.read("/sys/class/leds/lcd-backlight/max_brightness")).append('\n');
                Sensor l = sm.getDefaultSensor(Sensor.TYPE_LIGHT);
                float[] v;
                synchronized (sensorValues) { v = l == null ? null : sensorValues.get(System.identityHashCode(l)); }
                sb.append("light sensor  ").append(l == null ? "missing" : v == null ? "no reading yet" : v[0] + " lux").append('\n');
                sb.append("screen timeout").append(Settings.System.getInt(getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT, -1) / 1000).append(" s\n");
                sb.append("framebuffer   ").append(Util.read("/sys/class/graphics/fb0/modes")).append(", bpp ").append(Util.read("/sys/class/graphics/fb0/bits_per_pixel")).append('\n');
                sb.append("\nThe buttons change the system setting; the sysfs backlight must follow.\n");
            }
        };

        // ============================================================== GPU
        new Section("GPU", false) {
            String info = null;

            void buttons() {
                button("3D test ES 3", new Runnable() { public void run() { startActivity(new Intent(DiagActivity.this, GlTestActivity.class).putExtra("es", 3)); } });
                button("3D test ES 2", new Runnable() { public void run() { startActivity(new Intent(DiagActivity.this, GlTestActivity.class).putExtra("es", 2)); } });
                button("Probe again", new Runnable() { public void run() { info = null; enter(); } });
            }

            void enter() {
                if (info != null) return;
                info = "EGL/GLES probe running...";
                background(new Runnable() {
                    public void run() {
                        final String r = GpuInfo.probe();
                        h.post(new Runnable() { public void run() { info = r; refresh(); } });
                    }
                });
            }

            void render(StringBuilder sb) {
                ActivityManager a = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
                int v = a.getDeviceConfigurationInfo().reqGlEsVersion;
                sb.append("GLES version reported to the system: ").append(v >> 16).append('.').append(v & 0xffff).append('\n');
                sb.append("EGL driver: ").append(Util.read("/system/lib/egl/egl.cfg").replace('\n', ' ')).append('\n');
                sb.append("fbpresentd: ").append(Util.prop("init.svc.fbpresentd")).append("\n\n");
                sb.append(info);
            }
        };

        // ============================================================== Sensors
        new Section("Sensors", true) {
            void enter() { synchronized (sensorValues) { sensorValues.clear(); sensorCounts.clear(); } sensorsOn(); }
            void leave() { sensorsOff(); }

            void render(StringBuilder sb) {
                List<Sensor> list = sm.getSensorList(Sensor.TYPE_ALL);
                sb.append(list.size()).append(" sensors. Live values (proximity, light, steps: only when they change).\n\n");
                for (Sensor s : list) {
                    float[] v;
                    Long c;
                    synchronized (sensorValues) {
                        v = sensorValues.get(System.identityHashCode(s));
                        c = sensorCounts.get(System.identityHashCode(s));
                    }
                    sb.append("[").append(sensorType(s.getType())).append("] ").append(s.getName()).append(" — ").append(s.getVendor()).append('\n');
                    sb.append("    range ").append(s.getMaximumRange()).append(", resolution ").append(s.getResolution())
                      .append(", ").append(s.getPower()).append(" mA, min delay ").append(s.getMinDelay()).append(" us\n");
                    sb.append("    ");
                    if (s.getType() == Sensor.TYPE_SIGNIFICANT_MOTION) sb.append("(one-shot, not listened to)");
                    else if (v == null) sb.append("no value received");
                    else {
                        int n = Math.min(v.length, 6);
                        for (int i = 0; i < n; i++) sb.append(String.format("%10.3f", v[i]));
                        sb.append("   (").append(c).append(" events)");
                    }
                    sb.append("\n\n");
                }
            }
        };

        // ============================================================== Sound
        new Section("Sound", false) {
            void buttons() {
                button("Speaker beep 440 Hz", new Runnable() { public void run() { tone(AudioManager.STREAM_MUSIC, 440, "speaker"); } });
                button("Left/right beep", new Runnable() { public void run() { stereo(); } });
                button("Earpiece beep (call)", new Runnable() { public void run() { tone(AudioManager.STREAM_VOICE_CALL, 660, "earpiece"); } });
                button("Mic 3 s + playback", new Runnable() {
                    public void run() { note("recording 3 s... speak"); background(new Runnable() { public void run() { note(micTest(3000, true)); } }); }
                });
                button("Ringtone", new Runnable() {
                    public void run() {
                        final android.media.Ringtone r = RingtoneManager.getRingtone(DiagActivity.this, RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE));
                        if (r == null) { note("no ringtone"); return; }
                        r.play();
                        h.postDelayed(new Runnable() { public void run() { r.stop(); } }, 4000);
                        note("ringtone 4 s");
                    }
                });
            }

            void tone(final int stream, final int freq, final String what) {
                background(new Runnable() {
                    public void run() {
                        int prevMode = am.getMode();
                        if (stream == AudioManager.STREAM_VOICE_CALL) { am.setMode(AudioManager.MODE_IN_COMMUNICATION); am.setSpeakerphoneOn(false); }
                        int rate = 48000, n = rate * 2;
                        short[] buf = new short[n];
                        for (int i = 0; i < n; i++) buf[i] = (short) (Math.sin(2 * Math.PI * freq * i / rate) * 12000 * Math.min(1, Math.min(i, n - i) / 2000.0));
                        AudioTrack t = new AudioTrack(stream, rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, n * 2, AudioTrack.MODE_STATIC);
                        int w = t.write(buf, 0, n);
                        t.play();
                        note("beep " + what + ": AudioTrack state " + t.getState() + ", " + w + " samples written; can you hear it?");
                        SystemClock.sleep(2300);
                        t.release();
                        if (stream == AudioManager.STREAM_VOICE_CALL) am.setMode(prevMode);
                    }
                });
            }

            void stereo() {
                background(new Runnable() {
                    public void run() {
                        int rate = 48000, n = rate;
                        short[] buf = new short[n * 4];
                        for (int i = 0; i < n * 2; i++) {
                            short s = (short) (Math.sin(2 * Math.PI * 500 * i / rate) * 12000);
                            boolean left = i < n;
                            buf[2 * i] = left ? s : 0;
                            buf[2 * i + 1] = left ? 0 : s;
                        }
                        AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, buf.length * 2, AudioTrack.MODE_STATIC);
                        t.write(buf, 0, buf.length);
                        t.play();
                        note("1 s left then 1 s right (the Pixel 2 XL has two front speakers)");
                        SystemClock.sleep(2300);
                        t.release();
                    }
                });
            }

            void render(StringBuilder sb) {
                sb.append("mode          ").append(am.getMode()).append(", speakerphone ").append(am.isSpeakerphoneOn()).append(", wired headset ").append(am.isWiredHeadsetOn())
                  .append(", BT A2DP ").append(am.isBluetoothA2dpOn()).append(", mic muted ").append(am.isMicrophoneMute()).append('\n');
                sb.append("output        ").append(am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)).append(" Hz, buffer ").append(am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)).append(" frames\n\n");
                String[] names = { "call", "system", "ring", "music", "alarm", "notification" };
                int[] streams = { AudioManager.STREAM_VOICE_CALL, AudioManager.STREAM_SYSTEM, AudioManager.STREAM_RING, AudioManager.STREAM_MUSIC, AudioManager.STREAM_ALARM, AudioManager.STREAM_NOTIFICATION };
                for (int i = 0; i < names.length; i++)
                    sb.append("volume ").append(Util.pad(names[i], 14)).append(am.getStreamVolume(streams[i])).append(" / ").append(am.getStreamMaxVolume(streams[i])).append('\n');
                sb.append("\n== sound cards\n").append(Util.read("/proc/asound/cards")).append('\n');
                sb.append("\nHAL: ").append(new File("/system/lib/hw/audio.primary.taimen.so").exists() ? "audio.primary.taimen" : "default").append('\n');
            }
        };

        // ============================================================== Camera / flash
        new Section("Camera/Flash", false) {
            void buttons() {
                button("Flash ON", new Runnable() { public void run() { torch(true); } });
                button("Flash OFF", new Runnable() { public void run() { torch(false); } });
                button("Preview", new Runnable() { public void run() { startActivity(new Intent(DiagActivity.this, CameraPreviewActivity.class)); } });
                button("Open cameras", new Runnable() {
                    public void run() {
                        int n = Camera.getNumberOfCameras();
                        if (n == 0) note("no camera declared by the HAL");
                        for (int i = 0; i < n; i++) {
                            try {
                                Camera c = Camera.open(i);
                                Camera.Parameters p = c.getParameters();
                                note("camera " + i + " opened: " + p.getSupportedPictureSizes().size() + " sizes, flash " + p.getSupportedFlashModes());
                                c.release();
                            } catch (Exception e) { note("camera " + i + ": " + e.getMessage()); }
                        }
                    }
                });
            }

            void torch(boolean on) {
                String e1 = Util.write("/sys/class/leds/led:torch_0/brightness", on ? "100" : "0");
                String e2 = Util.write("/sys/class/leds/led:switch_0/brightness", on ? "1" : "0");
                note("flash " + (on ? "on" : "off") + " through sysfs: torch_0 " + (e1 == null ? "ok" : e1) + ", switch_0 " + (e2 == null ? "ok" : e2) + "; can you see it?");
            }

            void render(StringBuilder sb) {
                int n = Camera.getNumberOfCameras();
                sb.append("declared cameras: ").append(n).append(n == 0 ? "  (no camera HAL: the Camera app hides itself)" : "").append('\n');
                for (int i = 0; i < n; i++) {
                    Camera.CameraInfo ci = new Camera.CameraInfo();
                    Camera.getCameraInfo(i, ci);
                    sb.append("  camera ").append(i).append(ci.facing == Camera.CameraInfo.CAMERA_FACING_BACK ? " back" : " front").append(", orientation ").append(ci.orientation).append('\n');
                }
                sb.append("\n== LED flash (sysfs)\n");
                for (String l : Util.list("/sys/class/leds")) {
                    if (!l.contains("torch") && !l.contains("flash") && !l.contains("switch")) continue;
                    String d = "/sys/class/leds/" + l + "/";
                    sb.append(Util.pad(l, 16)).append("value ").append(Util.read(d + "brightness")).append(" / ").append(Util.read(d + "max_brightness"))
                      .append(new File(d + "brightness").canWrite() ? "  (writable)" : "  (read-only)").append('\n');
                }
            }
        };

        // ============================================================== LED / vibrator
        new Section("LED/Vibrator", false) {
            void buttons() {
                button("Red", new Runnable() { public void run() { led(255, 0, 0); } });
                button("Green", new Runnable() { public void run() { led(0, 255, 0); } });
                button("Blue", new Runnable() { public void run() { led(0, 0, 255); } });
                button("White", new Runnable() { public void run() { led(255, 255, 255); } });
                button("LED OFF", new Runnable() { public void run() { led(0, 0, 0); } });
                button("LED notification (turn the screen off)", new Runnable() {
                    public void run() {
                        Notification n = new Notification.Builder(DiagActivity.this)
                            .setSmallIcon(android.R.drawable.stat_notify_more).setContentTitle("Taimen Diag LED test")
                            .setContentText("turn the screen off: the LED must blink magenta")
                            .setLights(0xffff00ff, 500, 500).build();
                        n.flags |= Notification.FLAG_SHOW_LIGHTS;
                        nm.notify(42, n);
                        note("notification posted; KitKat only lights the LED with the screen off");
                    }
                });
                button("Cancel notification", new Runnable() { public void run() { nm.cancel(42); } });
                button("Vibrate 300 ms", new Runnable() { public void run() { vib.vibrate(300); note("vibration 300 ms"); } });
                button("Pattern", new Runnable() { public void run() { vib.vibrate(new long[] { 0, 100, 100, 100, 100, 400 }, -1); note("short-short-long pattern"); } });
            }

            /** Same sequence as the HAL: stop, set (brightness, on_off_ms), start (rgb_start). */
            void led(int r, int g, int b) {
                String[] c = { "red", "green", "blue" };
                int[] v = { r, g, b };
                String e = null;
                for (String x : c) Util.write("/sys/class/leds/" + x + "/rgb_start", "0");
                for (int i = 0; i < 3; i++) {
                    String e1 = Util.write("/sys/class/leds/" + c[i] + "/brightness", "" + v[i]);
                    String e2 = Util.write("/sys/class/leds/" + c[i] + "/on_off_ms", "1000 0");
                    if (e == null) e = e1 != null ? e1 : e2;
                }
                for (int i = 0; i < 3; i++) if (v[i] > 0) {
                    String e3 = Util.write("/sys/class/leds/" + c[i] + "/rgb_start", "1");
                    if (e == null) e = e3;
                }
                note("LED " + r + "," + g + "," + b + " : " + (e == null ? "written; can you see it?" : e));
            }

            void render(StringBuilder sb) {
                sb.append("vibrator present: ").append(vib.hasVibrator()).append('\n');
                sb.append("vibrator sysfs  : state ").append(Util.read("/sys/class/leds/vibrator/state")).append(", duration ").append(Util.read("/sys/class/leds/vibrator/duration")).append('\n');
                sb.append("\n== LED (sysfs)\n");
                String[] leds = { "red", "green", "blue", "lcd-backlight" };
                for (String l : leds) {
                    String d = "/sys/class/leds/" + l + "/";
                    sb.append(Util.pad(l, 14)).append(Util.read(d + "brightness")).append(" / ").append(Util.read(d + "max_brightness"))
                      .append(new File(d + "brightness").canWrite() ? "  (writable)" : "").append('\n');
                }
                sb.append("\nlights HAL: ").append(new File("/system/lib/hw/lights.taimen.so").exists() ? "lights.taimen present" : "missing").append('\n');
            }
        };

        // ============================================================== Clock
        new Section("Clock", true) {
            void buttons() {
                button("Alarm 10 s (wake-up)", new Runnable() { public void run() { scheduleAlarm(10000, "ELAPSED_REALTIME_WAKEUP"); note("alarm set in 10 s; you can turn the screen off"); } });
                button("Date/time settings", new Runnable() { public void run() { startActivity(new Intent(Settings.ACTION_DATE_SETTINGS)); } });
            }

            void render(StringBuilder sb) {
                Date now = new Date();
                sb.append("now           ").append(new SimpleDateFormat("EEEE d MMMM yyyy HH:mm:ss Z").format(now)).append('\n');
                sb.append("UTC           ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss").format(new Date(now.getTime() - TimeZone.getDefault().getOffset(now.getTime())))).append('\n');
                sb.append("time zone     ").append(TimeZone.getDefault().getID()).append(" (").append(TimeZone.getDefault().getDisplayName()).append(")\n");
                sb.append("auto time     ").append(Settings.Global.getInt(getContentResolver(), Settings.Global.AUTO_TIME, 0) == 1).append('\n');
                sb.append("auto zone     ").append(Settings.Global.getInt(getContentResolver(), Settings.Global.AUTO_TIME_ZONE, 0) == 1).append('\n');
                sb.append("24 h format   ").append(android.text.format.DateFormat.is24HourFormat(DiagActivity.this)).append('\n');
                sb.append("uptime        ").append(SystemClock.uptimeMillis() / 1000).append(" s (excluding sleep)\n");
                sb.append("elapsed       ").append(SystemClock.elapsedRealtime() / 1000).append(" s (since boot)\n");
                sb.append("RTC (sysfs)   ").append(Util.read("/sys/class/rtc/rtc0/since_epoch")).append(" s, ").append(Util.read("/sys/class/rtc/rtc0/name")).append('\n');
                sb.append("RTC offset    ").append(Util.read("/data/system/time/rtc_offset")).append(" s (saved when wall time is set)\n");
                sb.append("\ntest alarm    ").append(AlarmReceiver.result).append('\n');
                String next = Settings.System.getString(getContentResolver(), Settings.System.NEXT_ALARM_FORMATTED);
                sb.append("next alarm (Clock) ").append(next == null || next.length() == 0 ? "none" : next).append('\n');
            }
        };

        // ============================================================== Telephony
        new Section("Telephony", true) {
            String signal = "?", service = "?";
            final PhoneStateListener psl = new PhoneStateListener() {
                @Override public void onSignalStrengthsChanged(SignalStrength s) { signal = s.toString(); }
                @Override public void onServiceStateChanged(ServiceState s) { service = s.toString(); }
            };

            void enter() { tm.listen(psl, PhoneStateListener.LISTEN_SIGNAL_STRENGTHS | PhoneStateListener.LISTEN_SERVICE_STATE); }
            void leave() { tm.listen(psl, PhoneStateListener.LISTEN_NONE); }

            void buttons() {
                button("Dialer", new Runnable() { public void run() { startActivity(new Intent(Intent.ACTION_DIAL)); } });
                button("*#06#", new Runnable() { public void run() { note("IMEI: " + tm.getDeviceId()); } });
            }

            void render(StringBuilder sb) {
                sb.append("type          ").append(tm.getPhoneType() == TelephonyManager.PHONE_TYPE_GSM ? "GSM" : "" + tm.getPhoneType()).append('\n');
                sb.append("IMEI          ").append(tm.getDeviceId()).append(" (software ").append(tm.getDeviceSoftwareVersion()).append(")\n");
                sb.append("SIM           ").append(simState(tm.getSimState())).append('\n');
                sb.append("SIM operator  ").append(tm.getSimOperatorName()).append(" ").append(tm.getSimOperator()).append(", country ").append(tm.getSimCountryIso()).append('\n');
                sb.append("ICCID         ").append(tm.getSimSerialNumber()).append('\n');
                sb.append("IMSI          ").append(tm.getSubscriberId()).append('\n');
                sb.append("number        ").append(tm.getLine1Number()).append('\n');
                sb.append("network       ").append(tm.getNetworkOperatorName()).append(" ").append(tm.getNetworkOperator()).append(", ").append(netType(tm.getNetworkType()))
                  .append(tm.isNetworkRoaming() ? ", roaming" : "").append('\n');
                String[] ds = { "disconnected", "connecting", "connected", "suspended" };
                int d = tm.getDataState();
                sb.append("data          ").append(d >= 0 && d < ds.length ? ds[d] : "" + d).append('\n');
                String[] cs = { "idle", "ringing", "off-hook" };
                sb.append("call          ").append(cs[Math.max(0, Math.min(2, tm.getCallState()))]).append('\n');
                sb.append("signal        ").append(signal).append('\n');
                sb.append("service       ").append(service).append('\n');
                sb.append("\n== radio properties\n");
                String[] keys = { "gsm.sim.state", "gsm.operator.alpha", "gsm.operator.numeric", "gsm.network.type", "gsm.current.phone-type",
                    "gsm.version.ril-impl", "gsm.version.baseband", "persist.radio.taimen_sim", "init.svc.ril-daemon", "rild.libargs", "net.rmnet0.dns1" };
                for (String k : keys) sb.append(Util.pad(k, 28)).append(Util.prop(k)).append('\n');
            }
        };

        // ============================================================== Wi-Fi / Bluetooth / network
        new Section("Wi-Fi/BT", true) {
            int found = 0;
            final BroadcastReceiver btr = new BroadcastReceiver() {
                public void onReceive(Context c, Intent i) {
                    if (BluetoothDevice.ACTION_FOUND.equals(i.getAction())) {
                        BluetoothDevice d = i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                        found++;
                        note("BT found: " + d.getName() + " " + d.getAddress());
                    } else if (WifiManager.SCAN_RESULTS_AVAILABLE_ACTION.equals(i.getAction())) {
                        note("Wi-Fi scan: " + wm.getScanResults().size() + " networks");
                    }
                }
            };

            void enter() {
                IntentFilter f = new IntentFilter(BluetoothDevice.ACTION_FOUND);
                f.addAction(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION);
                registerReceiver(btr, f);
            }

            void leave() { try { unregisterReceiver(btr); } catch (Exception e) { } }

            void buttons() {
                button("Scan Wi-Fi", new Runnable() { public void run() { note("Wi-Fi scan started: " + wm.startScan()); } });
                button("BT discovery", new Runnable() {
                    public void run() {
                        BluetoothAdapter ba = BluetoothAdapter.getDefaultAdapter();
                        found = 0;
                        note(ba == null ? "no Bluetooth" : "BT discovery started: " + ba.startDiscovery());
                    }
                });
                button("BT ON/OFF", new Runnable() {
                    public void run() {
                        BluetoothAdapter ba = BluetoothAdapter.getDefaultAdapter();
                        if (ba != null) note(ba.isEnabled() ? "disabling BT: " + ba.disable() : "enabling BT: " + ba.enable());
                    }
                });
            }

            void render(StringBuilder sb) {
                sb.append("== Wi-Fi\n");
                sb.append("enabled       ").append(wm.isWifiEnabled()).append('\n');
                WifiInfo w = wm.getConnectionInfo();
                if (w != null) {
                    int ip = w.getIpAddress();
                    sb.append("SSID          ").append(w.getSSID()).append("  BSSID ").append(w.getBSSID()).append('\n');
                    sb.append("signal        ").append(w.getRssi()).append(" dBm, ").append(w.getLinkSpeed()).append(" Mbit/s\n");
                    sb.append("IP            ").append(ip & 0xff).append('.').append(ip >> 8 & 0xff).append('.').append(ip >> 16 & 0xff).append('.').append(ip >>> 24).append('\n');
                    sb.append("MAC           ").append(w.getMacAddress()).append('\n');
                }
                sb.append("networks seen ").append(wm.getScanResults() == null ? 0 : wm.getScanResults().size()).append('\n');
                sb.append("wlan0         ").append(Util.read("/sys/class/net/wlan0/operstate")).append('\n');
                NetworkInfo a = cm.getActiveNetworkInfo();
                sb.append("active network").append(a == null ? "none" : a.getTypeName() + " " + a.getState()).append('\n');
                sb.append("\n== Bluetooth\n");
                BluetoothAdapter ba = BluetoothAdapter.getDefaultAdapter();
                if (ba == null) sb.append("no adapter\n");
                else {
                    sb.append("enabled       ").append(ba.isEnabled()).append(", state ").append(ba.getState()).append('\n');
                    sb.append("name/address  ").append(ba.getName()).append(" ").append(ba.getAddress()).append('\n');
                    sb.append("discovering   ").append(ba.isDiscovering()).append(", found ").append(found).append('\n');
                    if (ba.isEnabled()) for (BluetoothDevice d : ba.getBondedDevices()) sb.append("paired        ").append(d.getName()).append(' ').append(d.getAddress()).append('\n');
                }
                sb.append("\n== NFC\n").append(NfcAdapter.getDefaultAdapter(DiagActivity.this) == null ? "missing (no NFC HAL)" : "present").append('\n');
                sb.append("\n== interfaces\n");
                for (String i : Util.list("/sys/class/net")) sb.append(Util.pad(i, 12)).append(Util.read("/sys/class/net/" + i + "/operstate")).append("  ").append(Util.read("/sys/class/net/" + i + "/address")).append('\n');
            }
        };

        // ============================================================== GPS
        new Section("GPS", true) {
            String loc = "none", sats = "?";
            boolean on;
            final LocationListener ll = new LocationListener() {
                public void onLocationChanged(Location l) { loc = l.getProvider() + " " + l.getLatitude() + ", " + l.getLongitude() + " +/-" + l.getAccuracy() + " m, " + new Date(l.getTime()); }
                public void onStatusChanged(String p, int s, Bundle b) {}
                public void onProviderEnabled(String p) {}
                public void onProviderDisabled(String p) {}
            };
            final GpsStatus.Listener gl = new GpsStatus.Listener() {
                public void onGpsStatusChanged(int e) {
                    GpsStatus s = lm.getGpsStatus(null);
                    int n = 0, used = 0;
                    for (GpsSatellite x : s.getSatellites()) { n++; if (x.usedInFix()) used++; }
                    sats = n + " seen, " + used + " used, first fix " + s.getTimeToFirstFix() + " ms";
                }
            };

            void buttons() {
                button("Start", new Runnable() {
                    public void run() {
                        for (String p : lm.getProviders(true)) if (!p.equals(LocationManager.PASSIVE_PROVIDER)) lm.requestLocationUpdates(p, 1000, 0, ll);
                        lm.addGpsStatusListener(gl);
                        on = true;
                        note("listening to providers: " + lm.getProviders(true));
                    }
                });
                button("Stop", new Runnable() { public void run() { stop(); note("stopped"); } });
                button("Location settings", new Runnable() { public void run() { startActivity(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)); } });
            }

            void stop() { lm.removeUpdates(ll); lm.removeGpsStatusListener(gl); on = false; }
            void leave() { stop(); }

            void render(StringBuilder sb) {
                for (String p : lm.getAllProviders())
                    sb.append(Util.pad(p, 10)).append(lm.isProviderEnabled(p) ? "enabled" : "disabled").append('\n');
                if (!lm.getAllProviders().contains(LocationManager.GPS_PROVIDER)) sb.append("\nno GPS provider: the GPS HAL is missing.\n");
                sb.append("\nlistening    ").append(on).append('\n');
                sb.append("satellites   ").append(sats).append('\n');
                sb.append("location     ").append(loc).append('\n');
            }
        };

        // ============================================================== Storage
        new Section("Storage", false) {
            void render(StringBuilder sb) {
                String[] paths = { "/system", "/data", "/cache", "/persist", Environment.getExternalStorageDirectory().getPath() };
                for (String p : paths) {
                    try {
                        StatFs s = new StatFs(p);
                        long tot = s.getBlockCountLong() * s.getBlockSizeLong(), free = s.getAvailableBlocksLong() * s.getBlockSizeLong();
                        sb.append(Util.pad(p, 22)).append(Util.bytes(free)).append(" free / ").append(Util.bytes(tot)).append('\n');
                    } catch (Exception e) { sb.append(Util.pad(p, 22)).append("? ").append(e.getMessage()).append('\n'); }
                }
                sb.append("external storage: ").append(Environment.getExternalStorageState()).append('\n');
                sb.append("\n== mount points\n");
                for (String l : Util.read("/proc/mounts").split("\n"))
                    if (l.startsWith("/dev/block") || l.contains(" /storage") || l.contains("fuse")) sb.append(l).append('\n');
            }
        };

        // ============================================================== Features
        new Section("Features", false) {
            void render(StringBuilder sb) {
                FeatureInfo[] f = getPackageManager().getSystemAvailableFeatures();
                List<String> names = new ArrayList<String>();
                for (FeatureInfo x : f) names.add(x.name != null ? x.name : "OpenGL ES " + x.getGlEsVersion());
                java.util.Collections.sort(names);
                sb.append(names.size()).append(" features declared to the system:\n\n");
                for (String n : names) sb.append(n).append('\n');
                sb.append("\n== installed HALs (/system/lib/hw)\n");
                for (String l : Util.list("/system/lib/hw")) sb.append(l).append('\n');
            }
        };

        // ============================================================== Touch / keys
        new Section("Touch/keys", false) {
            String touch = "touch the screen with several fingers (text area)";

            void enter() {
                body.setOnTouchListener(new View.OnTouchListener() {
                    public boolean onTouch(View v, MotionEvent e) {
                        StringBuilder t = new StringBuilder();
                        t.append(e.getPointerCount()).append(" finger(s), pressure ").append(String.format("%.2f", e.getPressure())).append('\n');
                        for (int i = 0; i < e.getPointerCount(); i++)
                            t.append("  finger ").append(e.getPointerId(i)).append(": ").append((int) e.getX(i)).append(", ").append((int) e.getY(i))
                             .append("  size ").append(String.format("%.2f", e.getSize(i))).append('\n');
                        touch = t.toString();
                        refresh();
                        return true;
                    }
                });
            }

            void leave() { body.setOnTouchListener(null); }

            void render(StringBuilder sb) {
                sb.append("last key: ").append(lastKey).append("\n(volume, power...; back leaves the section)\n\n");
                sb.append(touch).append("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
                sb.append("== input devices\n");
                for (int id : android.view.InputDevice.getDeviceIds()) {
                    android.view.InputDevice d = android.view.InputDevice.getDevice(id);
                    if (d != null) sb.append(id).append(": ").append(d.getName()).append('\n');
                }
            }
        };
    }

    // ------------------------------------------------------------------ shared tests
    void scheduleAlarm(long delay, String kind) {
        AlarmManager a = (AlarmManager) getSystemService(ALARM_SERVICE);
        long due = SystemClock.elapsedRealtime() + delay;
        AlarmReceiver.ok = null;
        AlarmReceiver.result = "... scheduled (" + delay / 1000 + " s)";
        Intent i = new Intent(this, AlarmReceiver.class).putExtra("due", due).putExtra("kind", kind);
        a.setExact(AlarmManager.ELAPSED_REALTIME_WAKEUP, due, PendingIntent.getBroadcast(this, 1, i, PendingIntent.FLAG_UPDATE_CURRENT));
    }

    /** Records from the microphone, returns the level; plays the recording back when asked. */
    String micTest(int ms, boolean playback) {
        int rate = 48000;
        int min = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) return "FAIL: capture format refused (" + min + ")";
        AudioRecord r = new AudioRecord(MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, Math.max(min, rate));
        if (r.getState() != AudioRecord.STATE_INITIALIZED) { r.release(); return "FAIL: AudioRecord not initialized (no capture in the HAL?)"; }
        short[] buf = new short[rate * ms / 1000];
        r.startRecording();
        int got = 0;
        long t0 = SystemClock.elapsedRealtime();
        while (got < buf.length && SystemClock.elapsedRealtime() - t0 < ms + 2000) {
            int n = r.read(buf, got, Math.min(4096, buf.length - got));
            if (n <= 0) break;
            got += n;
        }
        r.stop();
        r.release();
        int peak = 0;
        double sum = 0;
        for (int i = 0; i < got; i++) { peak = Math.max(peak, Math.abs(buf[i])); sum += (double) buf[i] * buf[i]; }
        double rms = got == 0 ? 0 : Math.sqrt(sum / got);
        String res = (got == 0 ? "FAIL: nothing read" : peak == 0 ? "FAIL: absolute silence" : "OK") + " (" + got + " samples, peak " + peak + ", RMS " + (int) rms + ")";
        if (playback && got > 0) {
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, got * 2, AudioTrack.MODE_STATIC);
            t.write(buf, 0, got);
            t.play();
            SystemClock.sleep(ms + 300);
            t.release();
            res += ", played back";
        }
        return "microphone: " + res;
    }

    /** Active slot: KitKat does not import androidboot.slot_suffix as a property, read the command line. */
    static String slot() {
        String p = android.os.SystemProperties.get("ro.boot.slot_suffix", "");
        if (p.length() > 0) return p;
        for (String a : Util.read("/proc/cmdline").split(" "))
            if (a.startsWith("androidboot.slot_suffix=")) return a.substring(24);
        return "?";
    }

    static String simState(int s) {
        String[] n = { "unknown", "ABSENT", "PIN required", "PUK required", "network locked", "ready" };
        return s >= 0 && s < n.length ? n[s] : "" + s;
    }

    static String netType(int t) {
        String[] n = { "unknown", "GPRS", "EDGE", "UMTS", "CDMA", "EVDO 0", "EVDO A", "1xRTT", "HSDPA", "HSUPA", "HSPA", "iDEN", "EVDO B", "LTE", "eHRPD", "HSPA+" };
        return t >= 0 && t < n.length ? n[t] : "type " + t;
    }
}
