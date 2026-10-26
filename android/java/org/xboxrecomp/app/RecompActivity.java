package org.xboxrecomp.app;

import org.libsdl.app.SDLActivity;

/*
 * The activity every title's APK starts: SDL's, with the engine and the
 * title in one library. SDL is linked statically into libmain.so, so there
 * is no libSDL3.so to load first; SDLActivity calls SDL_main in it, which is
 * src/host/android_main.c.
 */
public class RecompActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
