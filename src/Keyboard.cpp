#include "Keyboard.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <cstdio>
#include <cstring>

#include <Logger.h>

#include "Gfx.h"

namespace Keyboard {
    static bool running = false;
    static bool dialog_system_ready = false;
    static const int SCE_COMMON_DIALOG_STATUS_CANCELLED = 3;
    static uint16_t buffer[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
    static uint16_t title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
    static char last_error[96] = "";
    std::string text;

    static void set_error(const char *msg, int code = 0) {
        if (code != 0) {
            snprintf(last_error, sizeof(last_error), "%s (0x%08X)", msg, code);
        } else {
            strncpy(last_error, msg, sizeof(last_error) - 1);
            last_error[sizeof(last_error) - 1] = '\0';
        }
    }

    static void utf8_to_utf16(const char *src, uint16_t *dst, size_t dst_words) {
        size_t i = 0;
        for (; src[i] != '\0' && i + 1 < dst_words; i++) {
            dst[i] = static_cast<uint16_t>(static_cast<unsigned char>(src[i]));
        }
        dst[i] = 0;
    }

    static void utf16_to_utf8(const uint16_t *src, std::string &out) {
        out.clear();
        for (size_t i = 0; src[i] != 0; i++) {
            if (src[i] < 0x80) {
                out.push_back(static_cast<char>(src[i]));
            }
        }
    }

    bool initSystem() {
        if (dialog_system_ready) {
            return true;
        }

        last_error[0] = '\0';
        int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
        CSPOT_LOG(info, "IME: sceSysmoduleLoadModule(IME) -> 0x%08X", ret);

        // AppUtil / common dialog config can return benign "already done"
        // codes on some firmwares. Those must NOT block the IME dialog, so we
        // log them but keep going. Only sceImeDialogInit failing is fatal.
        SceAppUtilInitParam appUtilParam;
        SceAppUtilBootParam appUtilBootParam;
        sceClibMemset(&appUtilParam, 0, sizeof(appUtilParam));
        sceClibMemset(&appUtilBootParam, 0, sizeof(appUtilBootParam));
        ret = sceAppUtilInit(&appUtilParam, &appUtilBootParam);
        CSPOT_LOG(info, "IME: sceAppUtilInit -> 0x%08X", ret);

        SceCommonDialogConfigParam dialogConfig;
        sceCommonDialogConfigParamInit(&dialogConfig);
        ret = sceCommonDialogSetConfigParam(&dialogConfig);
        CSPOT_LOG(info, "IME: sceCommonDialogSetConfigParam -> 0x%08X", ret);

        dialog_system_ready = true;
        return true;
    }

    const char *getLastError() {
        return last_error[0] != '\0' ? last_error : nullptr;
    }

    // Returns: 1 = IME shown, user confirmed (text set); 0 = IME shown but
    // cancelled/empty; -1 = IME unavailable (caller should fall back).
    static int try_ime(const std::string &title, bool password) {
        if (!initSystem()) {
            return -1;
        }

        text.clear();
        last_error[0] = '\0';
        sceClibMemset(buffer, 0, sizeof(buffer));
        utf8_to_utf16(title.c_str(), title_utf16, sizeof(title_utf16) / sizeof(title_utf16[0]));

        SceImeDialogParam param;
        sceImeDialogParamInit(&param);
        param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
        param.languagesForced = SCE_TRUE;
        param.type = SCE_IME_TYPE_DEFAULT;
        param.option = 0;
        param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
        if (password) {
            param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD;
        }
        param.title = title_utf16;
        param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
        param.inputTextBuffer = buffer;

        int ret = sceImeDialogInit(&param);
        CSPOT_LOG(info, "IME: sceImeDialogInit -> 0x%08X", ret);
        if (ret < 0) {
            set_error("IME init failed", ret);
            return -1;
        }

        running = true;
        bool confirmed = false;
        bool done = false;
        while (!done) {
            Gfx::presentDialogFrame();

            SceCommonDialogStatus status = sceImeDialogGetStatus();
            if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
                SceImeDialogResult result;
                sceClibMemset(&result, 0, sizeof(SceImeDialogResult));
                sceImeDialogGetResult(&result);

                if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
                    utf16_to_utf8(buffer, text);
                    confirmed = true;
                } else {
                    set_error("IME cancelled");
                }

                sceImeDialogTerm();
                running = false;
                done = true;
            }

            sceKernelDelayThread(10000);
        }

        // The dialog worked even if the user cancelled; never fall back then.
        return confirmed ? 1 : 0;
    }

    static const char *charset() {
        return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.@ ";
    }

    static bool controller_pressed(unsigned int &pressed, SceCtrlData &pad, SceCtrlData &old_pad) {
        if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
            return false;
        }
        pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);
        old_pad = pad;
        return true;
    }

    static std::string get_text_controller(const std::string &title, bool password) {
        static const char *chars = charset();
        const size_t charset_len = strlen(chars);

        char value[256] = "";
        size_t value_len = 0;
        size_t char_index = 0;

        SceCtrlData pad{};
        SceCtrlData old_pad{};
        bool pad_seeded = false;
        last_error[0] = '\0';

        auto draw = [&]() {
            Gfx::beginFrame();
            Gfx::clear(Gfx::COLOR_BG);
            Gfx::text(40, 30, 1.3f, Gfx::COLOR_TEXT, title.c_str());
            Gfx::text(40, 70, 0.8f, Gfx::COLOR_SUBTEXT,
                      "(controller entry - IME unavailable)");

            char shown[256];
            if (password) {
                memset(shown, '*', value_len);
                shown[value_len] = '\0';
            } else {
                strncpy(shown, value, sizeof(shown) - 1);
                shown[sizeof(shown) - 1] = '\0';
            }
            Gfx::rect(40, 110, 880, 48, Gfx::COLOR_PANEL);
            Gfx::textf(56, 122, 1.1f, Gfx::COLOR_TEXT, "%s", shown);

            Gfx::textf(40, 190, 1.6f, Gfx::COLOR_ACCENT, "[ %c ]",
                       chars[char_index]);

            Gfx::text(40, 280, 0.9f, Gfx::COLOR_SUBTEXT,
                      "Up/Down = change character");
            Gfx::text(40, 310, 0.9f, Gfx::COLOR_SUBTEXT, "Cross = add");
            Gfx::text(40, 340, 0.9f, Gfx::COLOR_SUBTEXT, "Triangle = backspace");
            Gfx::text(40, 370, 0.9f, Gfx::COLOR_SUBTEXT, "Circle = cancel");
            Gfx::text(40, 400, 0.9f, Gfx::COLOR_SUBTEXT, "Start = done");
            Gfx::endFrame();
        };

        draw();

        bool done = false;
        bool cancelled = false;
        while (!done) {
            unsigned int pressed = 0;
            if (!controller_pressed(pressed, pad, old_pad)) {
                sceKernelDelayThread(10000);
                continue;
            }

            if (!pad_seeded) {
                pad_seeded = true;
                continue;
            }

            if (pressed & SCE_CTRL_UP) {
                char_index = (char_index + charset_len - 1) % charset_len;
                draw();
            } else if (pressed & SCE_CTRL_DOWN) {
                char_index = (char_index + 1) % charset_len;
                draw();
            } else if (pressed & SCE_CTRL_CROSS) {
                if (value_len + 1 < sizeof(value)) {
                    value[value_len++] = chars[char_index];
                    value[value_len] = '\0';
                    draw();
                }
            } else if (pressed & SCE_CTRL_TRIANGLE) {
                if (value_len > 0) {
                    value_len--;
                    value[value_len] = '\0';
                    draw();
                }
            } else if (pressed & SCE_CTRL_CIRCLE) {
                cancelled = true;
                done = true;
            } else if (pressed & SCE_CTRL_START) {
                done = true;
            }

            sceKernelDelayThread(10000);
        }

        if (cancelled) {
            set_error("Entry cancelled");
            return std::string();
        }
        return std::string(value);
    }

    std::string GetText(const std::string &title, bool password) {
        const int r = try_ime(title, password);
        if (r >= 0) {
            // IME dialog ran (text may be empty if the user cancelled).
            return text;
        }

        CSPOT_LOG(info, "IME unavailable, using controller entry (%s)",
                  last_error[0] ? last_error : "no error");
        return get_text_controller(title, password);
    }
}  // namespace Keyboard
