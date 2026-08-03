#include "cfg.h"
#include "net.h"
#include "agent.h"
#include "py.h"
#include "sesame.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include <wolfssh/ssh.h>

#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>

static const char *TAG = "sshd";

#define SSH_PORT      22
#define HOSTKEY_NVS   "ssh.hostkey"
#define MAX_HOSTKEY   256
#define SSH_LINE_MAX      512

static WOLFSSH_CTX *s_ctx;
static bool s_session_open;

static int load_or_make_hostkey(uint8_t *der, size_t cap, size_t *out_len)
{
    nvs_handle_t h;
    if (nvs_open("sesame", NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }

    size_t len = cap;
    if (nvs_get_blob(h, HOSTKEY_NVS, der, &len) == ESP_OK && len > 0) {
        nvs_close(h);
        *out_len = len;
        ESP_LOGI(TAG, "host key loaded (%u bytes)", (unsigned)len);
        return 0;
    }

    ESP_LOGI(TAG, "no host key yet, generating one");

    WC_RNG rng;
    ecc_key key;
    int rc = -1;

    if (wc_InitRng(&rng) != 0) {
        nvs_close(h);
        return -1;
    }
    if (wc_ecc_init(&key) != 0) {
        wc_FreeRng(&rng);
        nvs_close(h);
        return -1;
    }
    if (wc_ecc_make_key(&rng, 32, &key) == 0) {
        int sz = wc_EccKeyToDer(&key, der, cap);
        if (sz > 0) {
            *out_len = sz;
            if (nvs_set_blob(h, HOSTKEY_NVS, der, sz) == ESP_OK) {
                nvs_commit(h);
            }
            ESP_LOGI(TAG, "host key generated (%d bytes)", sz);
            rc = 0;
        }
    }

    wc_ecc_free(&key);
    wc_FreeRng(&rng);
    nvs_close(h);
    return rc;
}

static int auth_cb(byte type, WS_UserAuthData *data, void *ctx)
{
    (void)ctx;

    if (type != WOLFSSH_USERAUTH_PASSWORD) {
        return WOLFSSH_USERAUTH_FAILURE;
    }

    const char *want_user = cfg_get(CFG_SSH_USER, "sesame");
    const char *want_pass = cfg_get(CFG_SSH_PASS, "");

    if (!want_pass[0]) {
        ESP_LOGW(TAG, "login refused: no ssh.pass set");
        return WOLFSSH_USERAUTH_FAILURE;
    }

    size_t ulen = strlen(want_user), plen = strlen(want_pass);
    if (data->username == NULL ||
        data->usernameSz != ulen ||
        memcmp(data->username, want_user, ulen) != 0) {
        return WOLFSSH_USERAUTH_INVALID_USER;
    }
    if (data->sf.password.passwordSz != plen ||
        memcmp(data->sf.password.password, want_pass, plen) != 0) {
        return WOLFSSH_USERAUTH_INVALID_PASSWORD;
    }
    return WOLFSSH_USERAUTH_SUCCESS;
}

typedef struct {
    WOLFSSH *ssh;
    bool     dead;
} ssh_out_t;

static void ssh_write(sesame_out_t *o, const char *data, size_t len)
{
    ssh_out_t *s = o->ctx;
    if (s->dead) {
        return;
    }

    const char *p = data;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            if (i > start &&
                wolfSSH_stream_send(s->ssh, (byte *)(p + start), i - start) <= 0) {
                s->dead = true;
                return;
            }
            if (wolfSSH_stream_send(s->ssh, (byte *)"\r\n", 2) <= 0) {
                s->dead = true;
                return;
            }
            start = i + 1;
        }
    }
    if (start < len &&
        wolfSSH_stream_send(s->ssh, (byte *)(p + start), len - start) <= 0) {
        s->dead = true;
    }
}

static void ssh_say(ssh_out_t *s, const char *text)
{
    sesame_out_t o = { .write = ssh_write, .ctx = s };
    sesame_write(&o, text);
}

static void ssh_write_crlf(sesame_out_t *o, const char *data, size_t len)
{
    // Expand into a buffer and send in chunks. Sending a byte at a time meant
    // one SSH packet per character, which for the banner alone was hundreds of
    // round trips and dropped the session before it finished drawing.
    ssh_out_t *out = o->ctx;
    char buf[256];
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (n + 2 >= sizeof(buf)) {
            buf[n] = '\0';
            ssh_say(out, buf);
            n = 0;
        }
        if (data[i] == '\n') {
            buf[n++] = '\r';
        }
        buf[n++] = data[i];
    }
    if (n) {
        buf[n] = '\0';
        ssh_say(out, buf);
    }
}


static void session(int sock)
{
    WOLFSSH *ssh = wolfSSH_new(s_ctx);
    if (!ssh) {
        close(sock);
        return;
    }

    wolfSSH_set_fd(ssh, sock);

    if (wolfSSH_accept(ssh) != WS_SUCCESS) {
        ESP_LOGW(TAG, "handshake failed");
        wolfSSH_free(ssh);
        close(sock);
        return;
    }

    ssh_out_t out = { .ssh = ssh, .dead = false };
    sesame_out_t sink = { .write = ssh_write, .ctx = &out };

    char prompt[48];
    snprintf(prompt, sizeof(prompt), "%s> ", cfg_get(CFG_DEV_NAME, "sesame"));

    // The banner writes plain newlines; a raw SSH terminal needs carriage
    // returns too, so it goes through a small translating sink.
    sesame_out_t crlf = { .write = ssh_write_crlf, .ctx = &out };
    sesame_banner(&crlf, NULL);
    ssh_say(&out, "  type 'help' for commands, 'agent' to talk to it, "
                  "'exit' to leave\r\n\r\n");
    ssh_say(&out, prompt);

    // Conversation mode. In it, every line goes to the model instead of the
    // command table, which is what makes SSH a usable way to actually work
    // with the agent rather than only to poke at the device.
    bool repl = false;
    bool pyrepl = false;
    char pybuf[1024];
    int  pylen = 0;
    char agent_prompt[64];
    snprintf(agent_prompt, sizeof(agent_prompt), "%s ask> ",
             cfg_get(CFG_DEV_NAME, "sesame"));

    char line[SSH_LINE_MAX];
    int len = 0;
    byte ch;

    while (!out.dead) {
        int n = wolfSSH_stream_read(ssh, &ch, 1);
        if (n <= 0) {
            break;
        }

        if (ch == '\r' || ch == '\n') {
            ssh_say(&out, "\r\n");
            line[len] = '\0';

            if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
                if (pyrepl && pylen == 0) {
                    pyrepl = false;
                    ssh_say(&out, "left python\r\n");
                    len = 0;
                    ssh_say(&out, prompt);
                    continue;
                }
                if (repl) {
                    repl = false;
                    ssh_say(&out, "left conversation mode\r\n");
                    len = 0;
                    ssh_say(&out, prompt);
                    continue;
                }
                ssh_say(&out, "bye\r\n");
                break;
            }
            if (pyrepl) {
                if (pylen == 0 && (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0
                                   || strcmp(line, "exit()") == 0)) {
                    pyrepl = false;
                    ssh_say(&out, "left python\r\n");
                } else if (line[0] == '\0') {
                    if (pylen) {
                        py_run_interactive(pybuf, &crlf);
                        pylen = 0; pybuf[0] = '\0';
                    }
                } else {
                    int n = strlen(line);
                    if (pylen + n + 2 < (int)sizeof(pybuf)) {
                        memcpy(pybuf + pylen, line, n);
                        pylen += n;
                        pybuf[pylen++] = '\n';
                        pybuf[pylen] = '\0';
                    }
                    bool cont = (n && line[n-1] == ':') || line[0] == ' ' || line[0] == '\t';
                    if (!cont) {
                        py_run_interactive(pybuf, &crlf);
                        pylen = 0; pybuf[0] = '\0';
                    }
                }
            } else if (len > 0) {
                if (repl) {
                    agent_ask(line, &crlf);
                } else {
                    agent_take_repl_request();   // drop anything another transport left
                    sesame_exec(line, &sink);
                    if (agent_take_repl_request()) {
                        repl = true;
                        ssh_say(&out, "conversation mode. ask in plain language; "
                                      "'exit' returns to the shell.\r\n");
                    } else if (py_take_repl_request()) {
                        pyrepl = true; pylen = 0; pybuf[0] = '\0';
                        ssh_say(&out, "'exit' returns to the shell.\r\n");
                    }
                }
            }
            len = 0;
            ssh_say(&out, pyrepl ? (pylen ? "... " : ">>> ")
                                 : (repl ? agent_prompt : prompt));
            continue;
        }

        if (ch == 0x7f || ch == 0x08) {
            if (len > 0) {
                len--;
                ssh_say(&out, "\b \b");
            }
            continue;
        }
        if (ch == 0x03) {
            ssh_say(&out, "^C\r\n");
            len = 0;
            ssh_say(&out, prompt);
            continue;
        }
        if (ch == 0x04) {
            ssh_say(&out, "\r\n");
            break;
        }
        if (ch < 0x20 || ch > 0x7e) {
            continue;
        }

        if (len < SSH_LINE_MAX - 1) {
            line[len++] = ch;
            wolfSSH_stream_send(ssh, &ch, 1);
        }
    }

    wolfSSH_stream_exit(ssh, 0);
    wolfSSH_shutdown(ssh);
    wolfSSH_free(ssh);
    close(sock);
    ESP_LOGI(TAG, "session closed");
}

static void session_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    session(sock);
    s_session_open = false;
    vTaskDelete(NULL);
}

static void listener_task(void *arg)
{
    (void)arg;

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(SSH_PORT),
    };
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listener, 2) < 0) {
        ESP_LOGE(TAG, "bind/listen on port %d: errno %d", SSH_PORT, errno);
        close(listener);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on port %d as user '%s'", SSH_PORT,
             cfg_get(CFG_SSH_USER, "sesame"));

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int sock = accept(listener, (struct sockaddr *)&peer, &plen);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (s_session_open) {

            const char *busy = "sesame: a session is already open\r\n";
            send(sock, busy, strlen(busy), 0);
            close(sock);
            continue;
        }

        s_session_open = true;

        if (xTaskCreate(session_task, "ssh_sess", 16384,
                        (void *)(intptr_t)sock, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "no memory for a session task");
            s_session_open = false;
            close(sock);
        }
    }
}

esp_err_t sshd_start(void)
{
    if (s_ctx) {
        return ESP_OK;
    }
    if (!cfg_has(CFG_SSH_PASS)) {
        ESP_LOGI(TAG, "ssh disabled until a password is set (cfg set ssh.pass ...)");
        return ESP_OK;
    }

    if (wolfSSH_Init() != WS_SUCCESS) {
        ESP_LOGE(TAG, "wolfSSH_Init failed");
        return ESP_FAIL;
    }

    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!s_ctx) {
        ESP_LOGE(TAG, "wolfSSH_CTX_new failed");
        return ESP_FAIL;
    }

    static uint8_t der[MAX_HOSTKEY];
    size_t der_len = 0;
    if (load_or_make_hostkey(der, sizeof(der), &der_len) != 0) {
        ESP_LOGE(TAG, "no host key");
        wolfSSH_CTX_free(s_ctx);
        s_ctx = NULL;
        return ESP_FAIL;
    }

    if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, der, der_len,
                                         WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
        ESP_LOGE(TAG, "host key rejected");
        wolfSSH_CTX_free(s_ctx);
        s_ctx = NULL;
        return ESP_FAIL;
    }

    wolfSSH_SetUserAuth(s_ctx, auth_cb);
    wolfSSH_CTX_SetBanner(s_ctx, "sesame-agent-esp32\n");

    xTaskCreate(listener_task, "sshd", 4096, NULL, 3, NULL);
    return ESP_OK;
}

static int cmd_ssh(int argc, char **argv, sesame_out_t *out)
{
    if (argc >= 3 && strcmp(argv[1], "password") == 0) {
        cfg_set(CFG_SSH_PASS, argv[2]);
        sesame_write(out, "password set; reboot to start the server\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "user") == 0) {
        cfg_set(CFG_SSH_USER, argv[2]);
        sesame_write(out, "user set; reboot to apply\n");
        return 0;
    }

    char ip[16];
    net_ip(ip, sizeof(ip));
    sesame_printf(out, "server   %s\n", s_ctx ? "running on port 22" : "not started");
    sesame_printf(out, "user     %s\n", cfg_get(CFG_SSH_USER, "sesame"));
    sesame_printf(out, "password %s\n", cfg_has(CFG_SSH_PASS) ? "set" : "NOT SET (server disabled)");
    if (s_ctx) {
        sesame_printf(out, "connect  ssh %s@%s\n", cfg_get(CFG_SSH_USER, "sesame"), ip);
    }
    return 0;
}

// The device ships with admin/admin so that SSH works out of the box, which
// means the first useful thing anyone can do is change it. Naming it `passwd`
// rather than burying it under `ssh password` is the whole point: it is the
// name people already reach for.
static int cmd_passwd(int argc, char **argv, sesame_out_t *out)
{
    if (argc < 2) {
        sesame_printf(out,
            "usage: passwd <new password> [user]\n"
            "current user: %s%s\n",
            cfg_get(CFG_SSH_USER, "admin"),
            strcmp(cfg_get(CFG_SSH_PASS, ""), "admin") == 0
                ? "   (still the default password, worth changing)" : "");
        return 1;
    }
    if (strlen(argv[1]) < 4) {
        sesame_write(out, "passwd: pick at least 4 characters\n");
        return 1;
    }
    if (cfg_set(CFG_SSH_PASS, argv[1]) != ESP_OK) {
        sesame_write(out, "passwd: could not save\n");
        return 1;
    }
    if (argc > 2) {
        cfg_set(CFG_SSH_USER, argv[2]);
    }
    sesame_printf(out, "password changed for %s (takes effect on the next login)\n",
                  cfg_get(CFG_SSH_USER, "admin"));
    return 0;
}

static const sesame_cmd_t CMDS[] = {
    { "ssh", "ssh [user <name>|password <pw>]", "SSH server status and login", cmd_ssh },
    { "passwd", "passwd <new password> [user]", "change the SSH login password", cmd_passwd },
};

void cmd_ssh_register(void)
{
    for (unsigned i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        sesame_register(&CMDS[i]);
    }
}
