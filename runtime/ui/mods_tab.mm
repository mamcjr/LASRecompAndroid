// The menu's Mods page (Android): the game mods in mods/ (runtime/mods.c), each on or off, its own note (its .txt: the
// authors, the credits) as the description, and Import a mod folder (LASRecompActivity.importMod: the Android folder
// picker; the folder is copied into mods/). On or off applies when the game starts again.
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "../mods.h"
#include "../settings.h"
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_msg;  // the last import's outcome (set from the activity's thread)
static bool g_busy;

extern "C" JNIEXPORT void JNICALL Java_com_lasrecomp_LASRecompActivity_nativeModImportDone(JNIEnv *env, jclass, jstring message) {
    const char *text = env->GetStringUTFChars(message, NULL);
    pthread_mutex_lock(&g_lock);
    g_msg = text ? text : "";
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    if (text) env->ReleaseStringUTFChars(message, text);
}

static void import_mod(void) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetStaticMethodID(c, "importMod", "()V");
    bool called = false;
    if (m) env->CallStaticVoidMethod(c, m), called = true;
    if (env->ExceptionCheck()) env->ExceptionClear(), called = false;
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
    pthread_mutex_lock(&g_lock);
    g_busy = called;
    g_msg = called ? "" : "Could not open the folder picker.";
    pthread_mutex_unlock(&g_lock);
}

extern "C" void mods_tab_draw(float scale) {
    opt_begin("##mods", scale);
    opt_section("Mods");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Mods replace the game's files: each is a folder with a romfs folder inside, as for Eden "
                                 "or Ryujinx. Turning one on or off applies when the game starts again: Quit -> Return "
                                 "to start, then Play. A translation replaces one of the game's languages - usually US "
                                 "English: keep Language (Controls page) on Auto or English."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    static ModInfo mods[MODS_MAX];
    static int n = -1;
    static double at;
    double now = SDL_GetTicks() / 1000.0;
    if (n < 0 || now - at > 1) n = mods_list(mods, MODS_MAX), at = now;  // (once a second: a copy can land meanwhile)
    if (!n) ImGui::TextDisabled("%s", tr("No mods yet."));
    for (int i = 0; i < n; i++) {
        static char notes[MODS_MAX][1600];
        static char note_of[MODS_MAX][96];
        if (strcmp(note_of[i], mods[i].name)) {  // (read once per mod)
            mods_note(mods[i].name, notes[i], sizeof notes[i]);
            snprintf(note_of[i], sizeof note_of[i], "%s", mods[i].name);
        }
        bool on = mods[i].on;
        if (opt_toggle(mods[i].name, &on, true, notes[i])) {
            mods_set(mods[i].name, on);
            mods[i].on = on;
            settings_save();
        }
    }
    ImGui::Spacing();
    pthread_mutex_lock(&g_lock);
    bool busy = g_busy;
    std::string msg = g_msg;
    pthread_mutex_unlock(&g_lock);
    ImGui::BeginDisabled(busy);
    if (opt_button("Import a mod folder", "Pick the mod's folder (the one with romfs inside, already extracted from its "
                                          "ZIP / RAR): it is copied into the app. Or copy it from a computer into "
                                          "Android/data/com.lasrecomp.android/files/mods/."))
        import_mod();
    ImGui::EndDisabled();
    if (busy) ImGui::TextDisabled("%s", tr("Copying the mod..."));
    else if (msg.size() > 1) {  // (from the activity: "1|<name>" imported, "0|<reason>" failed, "2|" cancelled)
        char line[400];
        if (msg[0] == '1') snprintf(line, sizeof line, tr("Imported %s. It is on, from the next start of the game."), msg.c_str() + 2);
        else if (msg[0] == '2') snprintf(line, sizeof line, "%s", tr("Import cancelled."));
        else snprintf(line, sizeof line, tr("Import failed: %s"), tr(msg.c_str() + 2));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(line);
        ImGui::PopTextWrapPos();
    }
    opt_end();
}
#endif
