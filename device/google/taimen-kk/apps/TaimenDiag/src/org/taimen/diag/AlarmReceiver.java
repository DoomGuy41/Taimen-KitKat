package org.taimen.diag;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;

/** Receives the test alarm: records how late it is compared with the requested time. */
public class AlarmReceiver extends BroadcastReceiver {
    static volatile String result = "not started yet";
    static volatile Boolean ok = null;

    @Override
    public void onReceive(Context c, Intent i) {
        long due = i.getLongExtra("due", 0);
        long late = SystemClock.elapsedRealtime() - due;
        ok = late > -1000 && late < 15000;
        result = "received " + (late >= 0 ? late + " ms after" : -late + " ms before") + " the due time ("
                + i.getStringExtra("kind") + "): " + (ok ? "OK" : "FAIL");
    }
}
