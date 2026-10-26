package org.xboxrecomp.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.widget.EditText;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/*
 * The app's first screen: where the game is. The APK carries no game; the
 * player's own copy is found on the device and read where it is.
 *
 * In order:
 *   1. A folder already chosen (and still holding the game) -- start.
 *   2. A copy in the app's own folder (<external files>/game, which is where
 *      `adb push` puts one) -- start.
 *   3. Otherwise ask for all-files access, then look for default.xbe whose
 *      SHA-1 is the one this APK was built from (meta-data xboxrecomp.xbe_sha1)
 *      under the usual places: one match starts, several are listed, none
 *      asks for a path.
 *
 * Nothing is written to the game's folder: E: and the saves are in the app's
 * own storage (android_main.c), so a ROM folder shared with other programs
 * is only read.
 */
public class SetupActivity extends Activity {
    static final String PREFS = "xboxrecomp";
    static final String KEY_GAME = "game_dir";
    private static final int MAX_DEPTH = 4;

    private boolean askedForAccess;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
    }

    @Override
    protected void onResume() {
        super.onResume();
        choose();
    }

    private String expectedSha1() {
        try {
            ApplicationInfo ai = getPackageManager().getApplicationInfo(
                getPackageName(), PackageManager.GET_META_DATA);
            if (ai.metaData != null) {
                String s = ai.metaData.getString("xboxrecomp.xbe_sha1");
                if (s != null && s.length() == 40)
                    return s.toLowerCase();
            }
        } catch (PackageManager.NameNotFoundException e) {
            // fall through
        }
        return null;
    }

    private static boolean hasGame(String dir) {
        return dir != null && new File(dir, "default.xbe").isFile();
    }

    private void choose() {
        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        String saved = p.getString(KEY_GAME, null);
        if (hasGame(saved)) {
            start(saved);
            return;
        }
        File own = getExternalFilesDir(null);
        if (own != null && hasGame(new File(own, "game").getPath())) {
            start(new File(own, "game").getPath());
            return;
        }
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            if (askedForAccess) {
                askForPath("Without access to the device's files the game cannot be found. "
                           + "Give the path of the folder holding default.xbe, or allow access.");
                return;
            }
            askedForAccess = true;
            new AlertDialog.Builder(this)
                .setTitle("Find your game")
                .setMessage("This app needs to read your own copy of the game (the folder with "
                            + "default.xbe). Allow access to files on the next screen. "
                            + "Nothing is written to that folder.")
                .setCancelable(false)
                .setPositiveButton("Continue", (d, w) -> {
                    Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                          Uri.parse("package:" + getPackageName()));
                    startActivity(i);
                })
                .setNegativeButton("Type a path", (d, w) -> askForPath(null))
                .show();
            return;
        }
        scan();
    }

    private void scan() {
        final String want = expectedSha1();
        List<String> found = new ArrayList<>();
        File root = Environment.getExternalStorageDirectory();
        look(new File(root, "roms"), 0, want, found);
        look(new File(root, "Roms"), 0, want, found);
        look(new File(root, "ROMs"), 0, want, found);
        look(new File(root, "Download"), 0, want, found);
        look(new File(root, "games"), 0, want, found);
        File storage = new File("/storage");
        File[] cards = storage.listFiles();
        if (cards != null) {
            for (File c : cards) {
                if (c.getName().equals("emulated") || c.getName().equals("self"))
                    continue;
                look(c, 0, want, found);       // an SD card
            }
        }
        if (found.size() == 1) {
            remember(found.get(0));
            start(found.get(0));
        } else if (found.isEmpty()) {
            askForPath("The game was not found under roms/, Download/, games/ or an SD card. "
                       + "Give the path of the folder holding default.xbe.");
        } else {
            final String[] items = found.toArray(new String[0]);
            new AlertDialog.Builder(this)
                .setTitle("Which copy?")
                .setCancelable(false)
                .setItems(items, (d, which) -> {
                    remember(items[which]);
                    start(items[which]);
                })
                .show();
        }
    }

    private void look(File dir, int depth, String want, List<String> found) {
        if (depth > MAX_DEPTH || dir == null || !dir.isDirectory())
            return;
        File xbe = new File(dir, "default.xbe");
        if (xbe.isFile() && (want == null || want.equals(sha1(xbe)))) {
            // Shared storage ignores case: roms/ and ROMs/ are one folder.
            boolean seen = false;
            for (String f : found)
                seen |= f.equalsIgnoreCase(dir.getPath());
            if (!seen)
                found.add(dir.getPath());
            return;
        }
        File[] kids = dir.listFiles();
        if (kids == null)
            return;
        for (File k : kids) {
            if (k.isDirectory() && !k.getName().startsWith("."))
                look(k, depth + 1, want, found);
        }
    }

    private static String sha1(File f) {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                md.update(buf, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest())
                sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    private void askForPath(String why) {
        final EditText path = new EditText(this);
        path.setSingleLine(true);
        path.setText(Environment.getExternalStorageDirectory().getPath() + "/");
        new AlertDialog.Builder(this)
            .setTitle("Where is the game?")
            .setMessage(why != null ? why : "The folder holding default.xbe.")
            .setView(path)
            .setCancelable(false)
            .setPositiveButton("Play", (d, w) -> {
                String dir = path.getText().toString().trim();
                if (hasGame(dir)) {
                    remember(dir);
                    start(dir);
                } else {
                    askForPath("There is no default.xbe in " + dir + ".");
                }
            })
            .setNegativeButton("Quit", (d, w) -> finish())
            .show();
    }

    private void remember(String dir) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_GAME, dir).apply();
    }

    private void start(String dir) {
        Intent i = new Intent(this, RecompActivity.class);
        i.putExtra(KEY_GAME, dir);
        startActivity(i);
        finish();
    }
}
