/*
 * kkueinput - 한글 입력 헬퍼
 *
 * raw mode CLI의 한글 IME 조합 문제 우회.
 * GTK3 입력창에서 조합 → 터미널에 텍스트 주입.
 *
 * 백엔드:
 *   (기본)      TIOCSTI — /dev/tty에 직접 주입 (커널 < 6.2)
 *   --tmux=NAME tmux send-keys — 원격/로컬 tmux 세션에 주입
 *
 * 사용법:
 *   ./kkueinput &                  # TIOCSTI (로컬)
 *   ./kkueinput --tmux=mysession   # tmux 세션에 전송
 */

#include <gtk/gtk.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef enum {
    BACKEND_TIOCSTI,
    BACKEND_TMUX,
} Backend;

typedef struct {
    Backend    backend;
    int        tty_fd;       /* TIOCSTI용 */
    char      *tmux_target;  /* tmux 세션 이름 */
    GtkWidget *entry;
    GtkWidget *window;
} AppState;

/* TIOCSTI로 텍스트 주입 */
static void
send_tiocsti (int tty_fd, const char *text)
{
    for (const char *p = text; *p; p++) {
        if (ioctl (tty_fd, TIOCSTI, p) < 0) {
            g_printerr ("TIOCSTI 실패: %s\n", g_strerror (errno));
            return;
        }
    }
}

/* tmux send-keys로 텍스트 전송 */
static void
send_tmux (const char *target, const char *text)
{
    /* 셸 이스케이프: 싱글쿼트 안에 넣고, 텍스트 내 싱글쿼트만 처리 */
    GString *cmd = g_string_new ("tmux send-keys -t '");
    g_string_append (cmd, target);
    g_string_append (cmd, "' -- '");

    for (const char *p = text; *p; p++) {
        if (*p == '\'')
            g_string_append (cmd, "'\\''");
        else
            g_string_append_c (cmd, *p);
    }

    g_string_append (cmd, "' Enter");

    int ret = system (cmd->str);
    if (ret != 0)
        g_printerr ("tmux send-keys 실패 (exit %d)\n", ret);

    g_string_free (cmd, TRUE);
}

static void
send_text (AppState *state, const char *text)
{
    switch (state->backend) {
    case BACKEND_TIOCSTI:
        send_tiocsti (state->tty_fd, text);
        send_tiocsti (state->tty_fd, "\r");
        break;
    case BACKEND_TMUX:
        send_tmux (state->tmux_target, text);
        break;
    }
}

static gboolean
flush_idle (gpointer user_data)
{
    AppState *state = user_data;
    GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (state->entry));
    const char *text = gtk_entry_buffer_get_text (buf);

    if (text[0] != '\0') {
        send_text (state, text);
        gtk_entry_buffer_set_text (buf, "", 0);
    }

    return G_SOURCE_REMOVE;
}

static gboolean
on_key_press (GtkWidget   *widget G_GNUC_UNUSED,
              GdkEventKey *event,
              gpointer     user_data)
{
    AppState *state = user_data;

    if (event->keyval == GDK_KEY_x &&
        (event->state & GDK_CONTROL_MASK)) {
        gtk_window_close (GTK_WINDOW (state->window));
        return TRUE;
    }

    if (event->keyval == GDK_KEY_Return ||
        event->keyval == GDK_KEY_KP_Enter) {
        g_timeout_add (50, flush_idle, state);
        return FALSE;
    }

    /* F11/F12: 폭 조절 */
    if (event->keyval == GDK_KEY_F12 || event->keyval == GDK_KEY_F11) {
        int w, h;
        gtk_window_get_size (GTK_WINDOW (state->window), &w, &h);
        w += (event->keyval == GDK_KEY_F12) ? 40 : -40;
        if (w < 200) w = 200;
        gtk_window_resize (GTK_WINDOW (state->window), w, h);
        return TRUE;
    }

    return FALSE;
}

static void
show_about (GtkWidget *parent)
{
    gtk_show_about_dialog (GTK_WINDOW (parent),
                           "program-name", "kkueinput",
                           "comments", "IME input helper for CLI programs.\n"
                                       "Injects composed text into the "
                                       "controlling terminal via TIOCSTI "
                                       "or tmux send-keys.\n"
                                       "\n"
                                       "── Keys ──────────────\n"
                                       "Enter ···········  Send\n"
                                       "Ctrl+X ·········  Close\n"
                                       "F11 / F12 ···  −/+ Width\n"
                                       "\n"
                                       "── Mouse (icon) ──────\n"
                                       "Left drag ······  Move\n"
                                       "Right click ····  Menu",
                           "logo-icon-name", "input-keyboard",
                           "website", "mailto:kkuepark@gmail.com",
                           "website-label", "kkuepark@gmail.com",
                           "license-type", GTK_LICENSE_MIT_X11,
                           NULL);
}

/* 아이콘 클릭: 좌클릭 → 드래그, 우클릭 → 메뉴 */
static void
on_icon_press (GtkEntry             *entry    G_GNUC_UNUSED,
               GtkEntryIconPosition  pos      G_GNUC_UNUSED,
               GdkEvent             *event,
               gpointer              user_data)
{
    AppState *state = user_data;
    GdkEventButton *btn = (GdkEventButton *) event;

    if (btn->button == 1) {
        gtk_window_begin_move_drag (GTK_WINDOW (state->window),
                                    btn->button,
                                    (gint) btn->x_root,
                                    (gint) btn->y_root,
                                    btn->time);
        return;
    }

    GtkWidget *menu = gtk_menu_new ();

    GtkWidget *item_close = gtk_menu_item_new_with_label ("Close  Ctrl+X");
    g_signal_connect_swapped (item_close, "activate",
                              G_CALLBACK (gtk_window_close), state->window);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_close);

    GtkWidget *item_about = gtk_menu_item_new_with_label ("About");
    g_signal_connect_swapped (item_about, "activate",
                              G_CALLBACK (show_about), state->window);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_about);

    gtk_widget_show_all (menu);
    gtk_menu_popup_at_pointer (GTK_MENU (menu), event);
}

static void
on_screen_changed (GtkWidget *widget,
                   GdkScreen *old_screen G_GNUC_UNUSED,
                   gpointer   user_data  G_GNUC_UNUSED)
{
    GdkScreen *screen = gtk_widget_get_screen (widget);
    GdkVisual *visual = gdk_screen_get_rgba_visual (screen);

    if (visual)
        gtk_widget_set_visual (widget, visual);
}

static void
app_activate (GtkApplication *app, gpointer user_data)
{
    AppState *state = user_data;

    state->window = gtk_application_window_new (app);
    gtk_window_set_title (GTK_WINDOW (state->window), "kkueinput");
    gtk_window_set_default_size (GTK_WINDOW (state->window), 400, -1);
    gtk_window_set_resizable (GTK_WINDOW (state->window), FALSE);
    gtk_window_set_keep_above (GTK_WINDOW (state->window), TRUE);
    gtk_window_set_decorated (GTK_WINDOW (state->window), FALSE);
    gtk_window_set_icon_name (GTK_WINDOW (state->window), "input-keyboard");
    gtk_widget_set_app_paintable (state->window, TRUE);

    on_screen_changed (state->window, NULL, NULL);
    g_signal_connect (state->window, "screen-changed",
                      G_CALLBACK (on_screen_changed), NULL);

    GtkCssProvider *css = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (css,
        "window { background-color: transparent; }"
        "box { background-color: transparent; }"
        "entry { background-color: rgba(30, 30, 30, 0.75);"
        "        color: #ffffff;"
        "        border: 1px solid rgba(100, 100, 100, 0.4); }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen (
        gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (css);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width (GTK_CONTAINER (box), 8);
    gtk_container_add (GTK_CONTAINER (state->window), box);

    /* 입력 필드 */
    state->entry = gtk_entry_new ();

    /* placeholder에 백엔드 표시 */
    if (state->backend == BACKEND_TMUX) {
        char ph[128];
        g_snprintf (ph, sizeof (ph), "→ tmux:%s", state->tmux_target);
        gtk_entry_set_placeholder_text (GTK_ENTRY (state->entry), ph);
    } else {
        gtk_entry_set_placeholder_text (GTK_ENTRY (state->entry), "Type and Enter");
    }

    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (state->entry),
                                       GTK_ENTRY_ICON_SECONDARY,
                                       "input-keyboard-symbolic");
    gtk_entry_set_icon_tooltip_text (GTK_ENTRY (state->entry),
                                     GTK_ENTRY_ICON_SECONDARY,
                                     "메뉴");
    g_signal_connect (state->entry, "icon-press",
                      G_CALLBACK (on_icon_press), state);
    gtk_box_pack_start (GTK_BOX (box), state->entry, FALSE, FALSE, 0);

    g_signal_connect (state->window, "key-press-event",
                      G_CALLBACK (on_key_press), state);

    gtk_widget_show_all (state->window);
}

static void
print_usage (const char *prog)
{
    g_printerr ("Usage: %s [--tmux=SESSION]\n"
                "\n"
                "  (no args)        TIOCSTI mode (inject into controlling tty)\n"
                "  --tmux=SESSION   tmux mode (send-keys to named session)\n",
                prog);
}

int
main (int argc, char *argv[])
{
    static AppState state = { .backend = BACKEND_TIOCSTI, .tty_fd = -1 };

    /* --tmux=SESSION 파싱 */
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix (argv[i], "--tmux=")) {
            state.backend = BACKEND_TMUX;
            state.tmux_target = argv[i] + 7;
            /* GtkApplication이 이 인자를 먹지 않게 제거 */
            for (int j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            i--;
        } else if (g_str_equal (argv[i], "--help") ||
                   g_str_equal (argv[i], "-h")) {
            print_usage (argv[0]);
            return 0;
        }
    }

    if (state.backend == BACKEND_TIOCSTI) {
        state.tty_fd = open ("/dev/tty", O_WRONLY);
        if (state.tty_fd < 0) {
            g_printerr ("/dev/tty 열기 실패: %s\n", g_strerror (errno));
            return 1;
        }
    }

    GtkApplication *app = gtk_application_new ("com.kkuepark.kkueinput",
                                               G_APPLICATION_FLAGS_NONE);
    g_signal_connect (app, "activate", G_CALLBACK (app_activate), &state);

    int status = g_application_run (G_APPLICATION (app), argc, argv);

    if (state.tty_fd >= 0)
        close (state.tty_fd);

    g_object_unref (app);
    return status;
}
