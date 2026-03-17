/*
 * kkueinput - 한글 입력 헬퍼
 *
 * raw mode CLI의 한글 IME 조합 문제 우회.
 * GTK3 입력창에서 조합 → 터미널에 텍스트 주입.
 *
 * 백엔드:
 *   --tty        TIOCSTI — /dev/tty에 직접 주입 (커널 < 6.2)
 *   --tmux=NAME  tmux send-keys — 원격/로컬 tmux 세션에 주입
 *
 * 사용법:
 *   ./kkueinput --tty &              # TIOCSTI (로컬)
 *   ./kkueinput --tmux=mysession     # 로컬 tmux 세션에 전송
 *   ./kkueinput --ssh=devsvr --tmux=work  # SSH 원격 tmux 세션에 전송
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
    char      *ssh_host;    /* SSH 호스트 (NULL이면 로컬) */
    gboolean   multiline;    /* 멀티라인 모드 여부 */
    GtkWidget *entry;        /* GtkEntry (싱글라인) */
    GtkWidget *textview;     /* GtkTextView (멀티라인) */
    GtkWidget *scroll;       /* GtkScrolledWindow */
    GtkWidget *toggle_btn;   /* +/- 토글 버튼 */
    GtkWidget *grip;         /* 멀티라인용 드래그/메뉴 핸들 */
    GtkWidget *multi_box;    /* 멀티라인 컨테이너 */
    GtkWidget *window;
    int        font_size;   /* 폰트 크기 (pt) */
    GtkCssProvider *css;    /* 동적 CSS */
} AppState;

static void apply_css (AppState *state);

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

/* 셸 이스케이프: 싱글쿼트 감싸기 */
static void
append_shell_quoted (GString *cmd, const char *s)
{
    g_string_append_c (cmd, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'')
            g_string_append (cmd, "'\\''");
        else
            g_string_append_c (cmd, *p);
    }
    g_string_append_c (cmd, '\'');
}

/* tmux send-keys로 텍스트 전송 (로컬 또는 SSH 경유) */
static void
send_tmux (const char *ssh_host, const char *target, const char *text)
{
    GString *cmd = g_string_new (NULL);

    if (ssh_host)
        g_string_append_printf (cmd, "ssh %s ", ssh_host);

    g_string_append (cmd, "tmux send-keys -t ");
    append_shell_quoted (cmd, target);
    g_string_append (cmd, " -- ");
    append_shell_quoted (cmd, text);
    g_string_append (cmd, " Enter");

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
        send_tmux (state->ssh_host, state->tmux_target, text);
        break;
    }
}

static gboolean
flush_idle (gpointer user_data)
{
    AppState *state = user_data;

    if (state->multiline) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer (
                                 GTK_TEXT_VIEW (state->textview));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds (buf, &start, &end);
        char *text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
        if (text[0] != '\0') {
            send_text (state, text);
            gtk_text_buffer_set_text (buf, "", 0);
        }
        g_free (text);
    } else {
        GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (state->entry));
        const char *text = gtk_entry_buffer_get_text (buf);
        if (text[0] != '\0') {
            send_text (state, text);
            gtk_entry_buffer_set_text (buf, "", 0);
        }
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
        if (state->multiline) {
            /* 멀티라인: Ctrl+Enter → 전송, Enter → 줄바꿈 */
            if (!(event->state & GDK_CONTROL_MASK))
                return FALSE;
        }
        g_timeout_add (50, flush_idle, state);
        return TRUE;
    }

    /* F5/F6: 폰트 크기 조절 */
    if (event->keyval == GDK_KEY_F5 || event->keyval == GDK_KEY_F6) {
        state->font_size += (event->keyval == GDK_KEY_F5) ? -1 : 1;
        if (state->font_size < 6) state->font_size = 6;
        if (state->font_size > 48) state->font_size = 48;
        apply_css (state);
        return TRUE;
    }

    /* F11/F12: 폭 조절 */
    if (event->keyval == GDK_KEY_F11 || event->keyval == GDK_KEY_F12) {
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
                                       "Injects composed text via TIOCSTI "
                                       "or tmux send-keys.\n"
                                       "\n"
                                       "── Usage ─────────────\n"
                                       "kkueinput --tty &\n"
                                       "kkueinput --tmux=SESSION\n"
                                       "kkueinput --ssh=HOST --tmux=S\n"
                                       "\n"
                                       "── Keys ──────────────\n"
                                       "Enter ···········  Send (single)\n"
                                       "Ctrl+Enter ····  Send (multi)\n"
                                       "Ctrl+X ·········  Close\n"
                                       "F5 / F6 ·······  −/+ Font\n"
                                       "F11 / F12 ···  −/+ Width\n"
                                       "\n"
                                       "── Mouse (⌨ icon) ────\n"
                                       "Left drag ······  Move\n"
                                       "Right click ····  Menu\n"
                                       "\n"
                                       "── Mouse (⊕/⊖ icon) ──\n"
                                       "Left click ·····  Toggle",
                           "logo-icon-name", "input-keyboard",
                           "website", "mailto:kkuepark@gmail.com",
                           "website-label", "kkuepark@gmail.com",
                           "license-type", GTK_LICENSE_MIT_X11,
                           NULL);
}

static void toggle_multiline (GtkButton *btn, gpointer user_data);

static void
show_popup_menu (AppState *state, GdkEvent *event)
{
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
on_icon_press (GtkEntry             *entry    G_GNUC_UNUSED,
               GtkEntryIconPosition  pos,
               GdkEvent             *event,
               gpointer              user_data)
{
    AppState *state = user_data;
    GdkEventButton *btn = (GdkEventButton *) event;

    /* SECONDARY (우) = 확장 토글 */
    if (pos == GTK_ENTRY_ICON_SECONDARY) {
        toggle_multiline (NULL, state);
        return;
    }

    /* PRIMARY (좌): 좌클릭 → 드래그, 우클릭 → 메뉴 */
    if (btn->button == 1) {
        gtk_window_begin_move_drag (GTK_WINDOW (state->window),
                                    btn->button,
                                    (gint) btn->x_root,
                                    (gint) btn->y_root,
                                    btn->time);
        return;
    }

    show_popup_menu (state, event);
}

/* 축소 버튼 클릭 */
static gboolean
on_toggle_press (GtkWidget      *widget G_GNUC_UNUSED,
                 GdkEventButton *event  G_GNUC_UNUSED,
                 gpointer        user_data)
{
    toggle_multiline (NULL, user_data);
    return TRUE;
}

/* 멀티라인용 grip: 좌드래그 → 이동, 우클릭 → 메뉴 */
static gboolean
on_grip_press (GtkWidget *widget G_GNUC_UNUSED,
               GdkEventButton *event,
               gpointer        user_data)
{
    AppState *state = user_data;

    if (event->button == 1) {
        gtk_window_begin_move_drag (GTK_WINDOW (state->window),
                                    event->button,
                                    (gint) event->x_root,
                                    (gint) event->y_root,
                                    event->time);
        return TRUE;
    }

    if (event->button == 3) {
        show_popup_menu (state, (GdkEvent *) event);
        return TRUE;
    }

    return FALSE;
}

static void
toggle_multiline (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    AppState *state = user_data;

    int w, h;
    gtk_window_get_size (GTK_WINDOW (state->window), &w, &h);

    if (!state->multiline) {
        /* 싱글 → 멀티: 텍스트 이전 */
        const char *text = gtk_entry_buffer_get_text (
                               gtk_entry_get_buffer (GTK_ENTRY (state->entry)));
        GtkTextBuffer *buf = gtk_text_view_get_buffer (
                                 GTK_TEXT_VIEW (state->textview));
        gtk_text_buffer_set_text (buf, text, -1);

        gtk_entry_set_icon_from_icon_name (GTK_ENTRY (state->entry),
                                           GTK_ENTRY_ICON_SECONDARY,
                                           "list-remove-symbolic");
        gtk_widget_hide (state->entry);
        gtk_widget_show_all (state->multi_box);
        gtk_window_set_resizable (GTK_WINDOW (state->window), TRUE);
        gtk_window_resize (GTK_WINDOW (state->window), w, 140);
        state->multiline = TRUE;
        apply_css (state);
        gtk_widget_grab_focus (state->textview);
    } else {
        /* 멀티 → 싱글: 텍스트 이전 (첫 줄만) */
        GtkTextBuffer *buf = gtk_text_view_get_buffer (
                                 GTK_TEXT_VIEW (state->textview));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds (buf, &start, &end);
        char *text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
        gtk_entry_buffer_set_text (
            gtk_entry_get_buffer (GTK_ENTRY (state->entry)), text, -1);
        g_free (text);

        gtk_entry_set_icon_from_icon_name (GTK_ENTRY (state->entry),
                                           GTK_ENTRY_ICON_SECONDARY,
                                           "list-add-symbolic");
        gtk_widget_hide (state->multi_box);
        gtk_widget_show (state->entry);
        gtk_window_set_resizable (GTK_WINDOW (state->window), FALSE);
        gtk_window_resize (GTK_WINDOW (state->window), w, 1);
        state->multiline = FALSE;
        gtk_widget_grab_focus (state->entry);
    }
}

static void
apply_css (AppState *state)
{
    char css_str[1024];
    g_snprintf (css_str, sizeof (css_str),
        "window { background-color: transparent; }"
        "box { background-color: transparent; }"
        "entry { background-color: rgba(30, 30, 30, 0.75);"
        "        color: #ffffff;"
        "        font-size: %dpt;"
        "        border: 1px solid rgba(100, 100, 100, 0.4); }"
        "textview { background-color: transparent;"
        "        font-size: %dpt; }"
        "textview text { background-color: rgba(30, 30, 30, 0.75);"
        "        color: #ffffff;"
        "        caret-color: #ffffff; }"
        "scrolledwindow { background-color: transparent;"
        "        border: none; }"
        ".multiline-box { background-color: rgba(30, 30, 30, 0.75);"
        "        border: 1px solid rgba(100, 100, 100, 0.4);"
        "        border-radius: 3px;"
        "        padding: 2px; }",
        state->font_size, state->font_size);
    gtk_css_provider_load_from_data (state->css, css_str, -1, NULL);

    if (state->entry)
        gtk_widget_queue_resize (state->entry);
    if (state->textview)
        gtk_widget_queue_resize (state->textview);
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

    state->css = gtk_css_provider_new ();
    apply_css (state);
    gtk_style_context_add_provider_for_screen (
        gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (state->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width (GTK_CONTAINER (box), 8);
    gtk_container_add (GTK_CONTAINER (state->window), box);

    /* 입력 영역: [입력위젯][+/- 버튼] */
    GtkWidget *input_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start (GTK_BOX (box), input_box, TRUE, TRUE, 0);

    /* GtkEntry (싱글라인, 기본) */
    state->entry = gtk_entry_new ();
    if (state->backend == BACKEND_TMUX) {
        char ph[256];
        if (state->ssh_host)
            g_snprintf (ph, sizeof (ph), "→ %s:tmux:%s",
                        state->ssh_host, state->tmux_target);
        else
            g_snprintf (ph, sizeof (ph), "→ tmux:%s", state->tmux_target);
        gtk_entry_set_placeholder_text (GTK_ENTRY (state->entry), ph);
    } else {
        gtk_entry_set_placeholder_text (GTK_ENTRY (state->entry), "Type and Enter");
    }
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (state->entry),
                                       GTK_ENTRY_ICON_PRIMARY,
                                       "input-keyboard-symbolic");
    gtk_entry_set_icon_tooltip_text (GTK_ENTRY (state->entry),
                                     GTK_ENTRY_ICON_PRIMARY,
                                     "Drag / Menu");
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (state->entry),
                                       GTK_ENTRY_ICON_SECONDARY,
                                       "list-add-symbolic");
    gtk_entry_set_icon_tooltip_text (GTK_ENTRY (state->entry),
                                     GTK_ENTRY_ICON_SECONDARY,
                                     "Expand");
    g_signal_connect (state->entry, "icon-press",
                      G_CALLBACK (on_icon_press), state);
    gtk_box_pack_start (GTK_BOX (input_box), state->entry, TRUE, TRUE, 0);

    /* GtkTextView (멀티라인, 숨김) */
    state->textview = gtk_text_view_new ();
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (state->textview),
                                 GTK_WRAP_WORD_CHAR);
    state->scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (state->scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request (state->scroll, -1, 120);
    gtk_container_add (GTK_CONTAINER (state->scroll), state->textview);
    /* 멀티라인 컨테이너: [grip][scroll][toggle_btn] — entry와 같은 스타일 */
    state->multi_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *multi_box = state->multi_box;
    GtkStyleContext *ctx = gtk_widget_get_style_context (multi_box);
    gtk_style_context_add_class (ctx, "multiline-box");
    gtk_box_pack_start (GTK_BOX (input_box), multi_box, TRUE, TRUE, 0);

    state->grip = gtk_event_box_new ();
    GtkWidget *grip_icon = gtk_image_new_from_icon_name (
                               "input-keyboard-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_container_add (GTK_CONTAINER (state->grip), grip_icon);
    gtk_widget_set_valign (state->grip, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (state->grip, "Drag / Menu");
    g_signal_connect (state->grip, "button-press-event",
                      G_CALLBACK (on_grip_press), state);
    gtk_box_pack_start (GTK_BOX (multi_box), state->grip, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (multi_box), state->scroll, TRUE, TRUE, 0);

    state->toggle_btn = gtk_event_box_new ();
    GtkWidget *toggle_icon = gtk_image_new_from_icon_name (
                                 "list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_container_add (GTK_CONTAINER (state->toggle_btn), toggle_icon);
    gtk_widget_set_valign (state->toggle_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (state->toggle_btn, "Collapse");
    g_signal_connect (state->toggle_btn, "button-press-event",
                      G_CALLBACK (on_toggle_press), state);
    gtk_box_pack_start (GTK_BOX (multi_box), state->toggle_btn,
                        FALSE, FALSE, 0);

    g_signal_connect (state->window, "key-press-event",
                      G_CALLBACK (on_key_press), state);
    g_signal_connect (state->textview, "key-press-event",
                      G_CALLBACK (on_key_press), state);

    gtk_widget_show_all (state->window);

    /* 멀티라인 컨테이너는 초기에 숨김 (show_all로 자식까지 realize된 후) */
    gtk_widget_hide (state->multi_box);
}

static void
print_usage (const char *prog)
{
    g_printerr ("Usage: %s <backend>\n"
                "\n"
                "Backends (required, choose one):\n"
                "  --tty            TIOCSTI mode (inject into controlling tty)\n"
                "  --tmux=SESSION   tmux mode (send-keys to named session)\n"
                "\n"
                "Options:\n"
                "  --ssh=HOST       run tmux via SSH (host or user@host)\n",
                prog);
}

int
main (int argc, char *argv[])
{
    static AppState state = { .backend = BACKEND_TIOCSTI, .tty_fd = -1,
                               .font_size = 11 };

    /* 인자 파싱 */
    gboolean backend_set = FALSE;

    for (int i = 1; i < argc; i++) {
        if (g_str_equal (argv[i], "--tty")) {
            state.backend = BACKEND_TIOCSTI;
            backend_set = TRUE;
            for (int j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            i--;
        } else if (g_str_has_prefix (argv[i], "--tmux=")) {
            state.backend = BACKEND_TMUX;
            state.tmux_target = argv[i] + 7;
            backend_set = TRUE;
            for (int j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            i--;
        } else if (g_str_has_prefix (argv[i], "--ssh=")) {
            state.ssh_host = argv[i] + 6;
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

    if (!backend_set) {
        print_usage (argv[0]);
        return 1;
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
