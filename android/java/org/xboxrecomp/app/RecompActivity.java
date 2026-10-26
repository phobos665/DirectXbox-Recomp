package org.xboxrecomp.app;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/*
 * The game: SDL's activity, with the engine and the title in one library.
 * SDL is linked statically into libmain.so, so there is no libSDL3.so to load
 * first; SDLActivity calls SDL_main in it, which is src/host/android_main.c.
 *
 * SetupActivity says where the game is (the extra "game_dir"); it reaches the
 * engine as RECOMP_GAME_DIR, set before SDL_main runs. Started without one
 * (adb, during development) the engine looks in the app's own folder.
 */
public class RecompActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle state) {
        String dir = getIntent() != null ? getIntent().getStringExtra(SetupActivity.KEY_GAME) : null;
        if (dir != null) {
            try {
                Os.setenv("RECOMP_GAME_DIR", dir, true);
            } catch (ErrnoException e) {
                Log.e("xboxrecomp", "cannot pass the game folder: " + e);
            }
        }
        super.onCreate(state);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
