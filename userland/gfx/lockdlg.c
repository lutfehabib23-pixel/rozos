/* rozOS — lockdlg: ফোল্ডার লক/আনলকের ছোট উইন্ডো (ধাপ ১৯ছ)
 *
 *   lockdlg lock FOLDER          secret দুবার চায়, তারপর FOLDER.rozlock বানায়
 *   lockdlg unlock FILE [-o]     secret ঠিক হলে ফোল্ডার ফেরে; -o = তারপর Files-এ খোলা
 *
 * Files আর ডেস্কটপের ডান-ক্লিক মেনু এটা চালায়। কাজ librozlock করে (help rozlock)।
 * ফেরত: 0 = হয়েছে, 1 = বাতিল, 2 = ভুল। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rozgfx.h>
#include <rozui.h>
#include <rozlock.h>

static ui_ctx U;
static int locking;
static char shown[256];
static long last_draw;

static void progress(void *ctx, uint64_t done, uint64_t total, const char *name)
{
    const struct ui_theme *t = ui_theme_get();
    long now = gfx_ticks_ms();
    int pct = total ? (int)(done * 100 / total) : 0;
    char line[300];
    (void)ctx;
    if (now - last_draw < 100 && done < total) return;
    last_draw = now;
    if (pct > 100) pct = 100;
    gfx_clear(U.s, t->window_bg);
    snprintf(line, sizeof(line), "%s \"%s\" ...", locking ? "Locking" : "Unlocking", shown);
    ui_label(U.s, 20, 30, line, t->text);
    ui_progress(U.s, 20, 70, U.w - 40, 22, pct);
    if (name) {
        snprintf(line, sizeof(line), "%.50s", name);
        ui_label(U.s, 20, 104, line, t->text_dim);
    }
    ui_label(U.s, 20, 140, "Please wait - do not turn off the computer.", t->text_dim);
    gfx_present();
}

static void open_in_files(const char *path)
{
    const char *argv[3];
    argv[0] = "files";
    argv[1] = path;
    argv[2] = NULL;
    ui_launch(argv, NULL);
}

int main(int argc, char **argv)
{
    char s1[256] = "", s2[256] = "", msg[200] = "", out[1100];
    const char *path;
    int open_after = 0, rc = 1, i;

    if (argc < 3 || (strcmp(argv[1], "lock") && strcmp(argv[1], "unlock"))) {
        fprintf(stderr, "usage: lockdlg lock FOLDER | lockdlg unlock FILE [-o]\n");
        return 2;
    }
    locking = argv[1][0] == 'l';
    path = argv[2];
    for (i = 3; i < argc; i++) if (!strcmp(argv[i], "-o")) open_after = 1;
    rozlock_display_name(path, shown, sizeof(shown));
    if (!locking && !rozlock_is_lockfile(path)) {
        char p2[1100];
        snprintf(p2, sizeof(p2), "%s" ROZLOCK_EXT, path);
        if (!rozlock_is_lockfile(p2)) {
            fprintf(stderr, "lockdlg: %s: not a locked folder\n", path);
            return 2;
        }
    }
    if (gfx_init_window(locking ? "Lock folder" : "Unlock folder", 460, locking ? 280 : 210) < 0) {
        fprintf(stderr, "lockdlg: needs the desktop\n");
        return 2;
    }
    ui_init(&U);
    U.want_focus = 1;
    while (ui_run(&U)) {
        const struct ui_theme *t = ui_theme_get();
        int go = 0, y = 18, r1, r2 = 0;
        char line[320];
        gfx_clear(U.s, t->window_bg);
        ui_draw_icon(U.s, 20, y, 32, locking ? "folder" : "locked-folder", locking ? 'f' : 'l');
        snprintf(line, sizeof(line), "%s: %.40s", locking ? "Folder" : "Locked folder", shown);
        ui_label(U.s, 64, y + 2, line, t->text);
        ui_label(U.s, 64, y + 20, locking ? "Only someone with the secret can open it."
                                         : "Write the secret to open it.", t->text_dim);
        y += 52;
        ui_label(U.s, 20, y + 4, "Secret:", t->text);
        r1 = ui_do_password(&U, 100, y, U.w - 120, s1, sizeof(s1));
        y += 34;
        if (locking) {
            ui_label(U.s, 20, y + 4, "Again:", t->text);
            r2 = ui_do_password(&U, 100, y, U.w - 120, s2, sizeof(s2));
            if (r1 == 2) ui_focus_next(&U);
            y += 34;
            ui_label(U.s, 20, y, "If you forget the secret, the files", GFX_RGB(240, 190, 90));
            ui_label(U.s, 20, y + 18, "can NEVER be recovered.", GFX_RGB(240, 190, 90));
            y += 44;
        }
        if (msg[0]) ui_label(U.s, 20, y, msg, GFX_RGB(240, 110, 100));
        if (ui_do_button(&U, U.w - 200, U.h - 44, 88, 28, locking ? "Lock" : "Unlock")) go = 1;
        if (ui_do_button(&U, U.w - 104, U.h - 44, 88, 28, "Cancel")) ui_quit(&U);
        if ((locking ? r2 : r1) == 2) go = 1;
        if (U.key == GFX_KEY_ESC) ui_quit(&U);
        if (!go) continue;

        if (!s1[0]) { snprintf(msg, sizeof(msg), "Write the secret first."); ui_redraw(&U); continue; }
        if (locking && strcmp(s1, s2)) {
            snprintf(msg, sizeof(msg), "The two secrets are not the same.");
            s2[0] = '\0';
            ui_redraw(&U);
            continue;
        }
        if (locking && strlen(s1) < 4) { snprintf(msg, sizeof(msg), "Use at least 4 characters."); ui_redraw(&U); continue; }
        if (!locking) {
            int c = rozlock_check(path, s1);
            if (c == ROZLOCK_EWRONG) {
                snprintf(msg, sizeof(msg), "Wrong secret - try again.");
                memset(s1, 0, sizeof(s1));
                ui_redraw(&U);
                continue;
            }
        }
        last_draw = 0;
        progress(NULL, 0, 1, NULL);
        {
            int r = locking ? rozlock_lock(path, s1, progress, NULL, out, sizeof(out))
                            : rozlock_unlock(path, s1, progress, NULL, out, sizeof(out));
            memset(s1, 0, sizeof(s1));
            memset(s2, 0, sizeof(s2));
            if (r == 0 || r == ROZLOCK_EPARTIAL) {
                rc = 0;
                if (r < 0) ui_msgbox(locking ? "Lock folder" : "Unlock folder", rozlock_strerror(r), UI_MB_OK | UI_MB_WARN);
                else if (locking) ui_msgbox("Lock folder", "The folder is locked.\nRemember your secret!", UI_MB_OK | UI_MB_INFO);
                if (!locking && open_after) open_in_files(out);
            } else {
                char m[300];
                snprintf(m, sizeof(m), "Could not %s \"%s\":\n%s", locking ? "lock" : "unlock", shown, rozlock_strerror(r));
                ui_msgbox(locking ? "Lock folder" : "Unlock folder", m, UI_MB_OK | UI_MB_ERROR);
                rc = 2;
            }
        }
        ui_quit(&U);
    }
    memset(s1, 0, sizeof(s1));
    memset(s2, 0, sizeof(s2));
    gfx_quit();
    return rc;
}
