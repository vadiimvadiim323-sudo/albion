// modules/ui/strings.hpp
#pragma once
// ---------------------------------------------------------------------------
// Русские строки через \u-эскейпы (не зависят от кодировки исходника)
// ---------------------------------------------------------------------------
// \u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 = "Настройки"
// \u043f\u0440\u0438\u0446\u0435\u043b\u0438\u0432\u0430\u043d\u0438\u044f = "прицеливания"
#define RU_WIN_TITLE   L"\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u043f\u0440\u0438\u0446\u0435\u043b\u0438\u0432\u0430\u043d\u0438\u044f"
#define RU_AIM_SPEED   L"\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c \u043f\u0440\u0438\u0446\u0435\u043b\u0430:"
#define RU_CAPTURE     L"\u041e\u0431\u043b\u0430\u0441\u0442\u044c \u0437\u0430\u0445\u0432\u0430\u0442\u0430:"
#define RU_BODY_PART   L"\u0427\u0430\u0441\u0442\u044c \u0442\u0435\u043b\u0430:"
#define RU_MODEL       L"\u041c\u043e\u0434\u0435\u043b\u044c:"
#define RU_HEAD        L"\u0413\u043e\u043b\u043e\u0432\u0430 (7.5%)"
#define RU_HEAD_Y_OFFSET L"Y offset (\u0433\u043e\u043b\u043e\u0432\u0430):"
#define RU_HEAD_Y_HINT L"+ = \u0432\u044b\u0448\u0435, - = \u043d\u0438\u0436\u0435"
#define RU_RESET_OFFSET L"\u0421\u0431\u0440\u043e\u0441"
#define RU_NECK        L"\u0428\u0435\u044f (30%)"
#define RU_BODY        L"\u0422\u0435\u043b\u043e (45%)"
#define RU_CLOSEST     L"\u0411\u043b\u0438\u0436\u0430\u0439\u0448\u0435\u0435"
#define RU_HEAD_SHORT    L"\u0413\u043e\u043b\u043e\u0432\u0430"
#define RU_NECK_SHORT    L"\u0428\u0435\u044f"
#define RU_BODY_SHORT    L"\u0422\u0435\u043b\u043e"
#define RU_CLOSEST_SHORT L"\u0411\u043b\u0438\u0436\u0430\u0439\u0448\u0435\u0435"
#define RU_INIT        L"\u0418\u043d\u0438\u0446\u0438\u0430\u043b\u0438\u0437\u0430\u0446\u0438\u044f..."
#define RU_NO_MODELS   L"\u041c\u043e\u0434\u0435\u043b\u0438 \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u044b"
#define RU_ERROR       L"\u041e\u0448\u0438\u0431\u043a\u0430"
#define RU_ON          L"\u0432\u043a\u043b"
#define RU_OFF         L"\u0432\u044b\u043a\u043b"
#define RU_PX          L"\u043f\u043a\u0441"
// "Порог уверенности:"
#define RU_CONFIDENCE  L"\u041f\u043e\u0440\u043e\u0433 \u0443\u0432\u0435\u0440\u0435\u043d\u043d\u043e\u0441\u0442\u0438:"
// "Способ наведения:"
#define RU_MOUSE_SCALE L"\u041c\u0430\u0441\u0448\u0442\u0430\u0431 \u043c\u044b\u0448\u0438:"
#define RU_AIM_METHOD  L"\u0421\u043f\u043e\u0441\u043e\u0431 \u043d\u0430\u0432\u0435\u0434\u0435\u043d\u0438\u044f:"
#define RU_METHOD_WINAPI  L"WinAPI (SendInput)"
#define RU_METHOD_ARDUINO L"Arduino HID"
// "Limit FPS:" / "Bez limita" / "Limit:"
#define RU_FPS_LIMIT   L"\u041b\u0438\u043c\u0438\u0442 FPS:"
#define RU_FPS_UNLIM   L"\u0411\u0435\u0437 \u043b\u0438\u043c\u0438\u0442\u0430"
#define RU_FPS_SHORT   L"\u041b\u0438\u043c\u0438\u0442"
// "Sposob zakhvata:" / "Nash (dirty-rects)" / "DirectX (kak v Aimmy)"
#define RU_CAP_METHOD  L"\u0421\u043f\u043e\u0441\u043e\u0431 \u0437\u0430\u0445\u0432\u0430\u0442\u0430:"
#define RU_CAP_OURS    L"dirty-rects"
#define RU_CAP_DIRECTX L"DirectX"
// "Traektoriya:"
#define RU_PATH        L"\u0422\u0440\u0430\u0435\u043a\u0442\u043e\u0440\u0438\u044f:"
// Vkladki i vizualy
#define RU_TAB_SETTINGS L"\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438"
#define RU_TAB_VISUALS  L"\u0412\u0438\u0437\u0443\u0430\u043b\u044b"
#define RU_TAB_AI       L"\u0418\u0418"
#define RU_TAB_CONFIG   L"\u041a\u043e\u043d\u0444\u0438\u0433"
#define RU_CONFIG_TITLE L"\u041a\u043e\u043d\u0444\u0438\u0433\u0443\u0440\u0430\u0446\u0438\u0438"
#define RU_CONFIG_NAME  L"\u0418\u043c\u044f \u043a\u043e\u043d\u0444\u0438\u0433\u0430:"
#define RU_DELETE_CONFIG L"\u0423\u0434\u0430\u043b\u0438\u0442\u044c"
#define RU_RENAME_CONFIG L"\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u0442\u044c"
#define RU_SAVE_CONFIG  L"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c"
#define RU_LOAD_CONFIG  L"\u0417\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c"
#define RU_CONFIG_SAVED       L"\u0421\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u043e: "
#define RU_CONFIG_SAVE_FAILED L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0441\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433"
#define RU_CONFIG_LOADED       L"\u0417\u0430\u0433\u0440\u0443\u0436\u0435\u043d\u043e: "
#define RU_CONFIG_LOAD_FAILED L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0437\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433"
#define RU_CONFIG_DELETED L"\u0423\u0434\u0430\u043b\u0435\u043d\u043e: "
#define RU_CONFIG_RENAMED L"\u041f\u0435\u0440\u0435\u0438\u043c\u0435\u043d\u043e\u0432\u0430\u043d\u043e: "
#define RU_CONFIG_ENTER_NAME L"\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u0438\u043c\u044f \u043a\u043e\u043d\u0444\u0438\u0433\u0430"
#define RU_CONFIG_BAD_NAME L"\u0418\u043c\u044f \u0441\u043e\u0434\u0435\u0440\u0436\u0438\u0442 \u043d\u0435\u0434\u043e\u043f\u0443\u0441\u0442\u0438\u043c\u044b\u0435 \u0441\u0438\u043c\u0432\u043e\u043b\u044b"
#define RU_VIS_REGION   L"\u041f\u043e\u043a\u0430\u0437\u044b\u0432\u0430\u0442\u044c \u043e\u0431\u043b\u0430\u0441\u0442\u044c \u0437\u0430\u0445\u0432\u0430\u0442\u0430"
#define RU_VIS_BOXES    L"\u041f\u043e\u043a\u0430\u0437\u044b\u0432\u0430\u0442\u044c \u0440\u0430\u0441\u043f\u043e\u0437\u043d\u0430\u043d\u043d\u044b\u0435 \u043e\u0431\u044a\u0435\u043a\u0442\u044b"
#define RU_VIS_STATS    L"\u041f\u043e\u043a\u0430\u0437\u044b\u0432\u0430\u0442\u044c \u0441\u0442\u0430\u0442\u0443\u0441"
#define RU_VIS_CONF     L"\u041f\u043e\u043a\u0430\u0437\u044b\u0432\u0430\u0442\u044c \u0443\u0432\u0435\u0440\u0435\u043d\u043d\u043e\u0441\u0442\u044c"
// "Клавиша наведения:"
// "Статус: "
#define RU_HUD_STATUS   L"\u0421\u0442\u0430\u0442\u0443\u0441: "
// "%hs  |  Инференс: %.1f мс  |  Объектов: %zu  |  Бинд: %ls"
#define RU_FMT_HUD      L"%hs  |  \u0418\u043d\u0444\u0435\u0440\u0435\u043d\u0441: %.1f \u043c\u0441"  \
                        L"  |  \u041e\u0431\u044a\u0435\u043a\u0442\u043e\u0432: %zu"  \
                        L"  |  \u0411\u0438\u043d\u0434: %ls"
// "Загрузка: %hs"
#define RU_FMT_HUD_LOAD L"\u0417\u0430\u0433\u0440\u0443\u0437\u043a\u0430: %ls"
#define RU_BIND_LABEL   L"\u041a\u043b\u0430\u0432\u0438\u0448\u0430 \u043d\u0430\u0432\u0435\u0434\u0435\u043d\u0438\u044f:"
// "Нажмите клавишу..."
#define RU_BIND_PRESS   L"\u041d\u0430\u0436\u043c\u0438\u0442\u0435 \u043a\u043b\u0430\u0432\u0438\u0448\u0443..."

#define RU_FMT_OVERLAY \
    L"FPS \u0437\u0430\u0445\u0432\u0430\u0442\u0430: %.0f\n" \
    L"\u0418\u043d\u0444\u0435\u0440\u0435\u043d\u0441: %.1f \u043c\u0441\n" \
    L"\u041a\u043e\u043d\u0432\u0435\u0439\u0435\u0440: %.1f \u043c\u0441\n" \
    L"\u041e\u0431\u044a\u0435\u043a\u0442\u043e\u0432: %zu"

// Строки формата для поля статистики
// "Захват: %.1f к/с\r\n"
#define RU_FMT_STATS \
    L"\u0417\u0430\u0445\u0432\u0430\u0442: %.1f \u043a/\u0441  |  " RU_FPS_SHORT L": %ls\r\n" \
    L"\u0411\u044d\u043a\u0435\u043d\u0434: %hs  |  " \
    L"\u0418\u043d\u0444\u0435\u0440\u0435\u043d\u0441: %.1f \u043c\u0441\r\n" \
    L"\u0417\u0430\u0434\u0435\u0440\u0436\u043a\u0430 \u043a\u043e\u043d\u0432\u0435\u0439\u0435\u0440\u0430: %.1f \u043c\u0441\r\n" \
    L"\u041e\u0431\u044a\u0435\u043a\u0442\u043e\u0432: %zu\r\n" \
    L"\u0411\u0438\u043d\u0434 \u043d\u0430\u0432\u0435\u0434\u0435\u043d\u0438\u044f: %ls\r\n" \
    L"\u0427\u0430\u0441\u0442\u044c \u0442\u0435\u043b\u0430: %ls  |  \u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c: %.0f%%\r\n" \
    L"\u041f\u043e\u0440\u043e\u0433: %.2f  |  \u041c\u0435\u0442\u043e\u0434: %ls\r\n" \
    RU_PATH L" %ls\r\n" \
    RU_CAP_METHOD L" %ls\r\n" \
    L"\u0412\u0445\u043e\u0434 \u0441\u0435\u0442\u0438: %d x %d\r\n" \
    L"\u0412\u044b\u0445\u043e\u0434: %d x %d  |  %hs\r\n" \
    L"\u041e\u0431\u043b\u0430\u0441\u0442\u044c \u0437\u0430\u0445\u0432\u0430\u0442\u0430: %u x %u " RU_PX L"\r\n" \
    L"[Esc] \u0432\u044b\u0445\u043e\u0434"

#define RU_FMT_LOADING \
    L"\u0417\u0430\u0445\u0432\u0430\u0442: %.1f \u043a/\u0441\r\n" \
    L"\u041c\u043e\u0434\u0435\u043b\u044c: %hs"

enum : int
{
    IDC_AIM_SPEED = 1001,
    IDC_CAPTURE_SIZE = 1002,
    IDC_SPEED_VALUE = 1003,
    IDC_CAPTURE_VALUE = 1004,
    IDC_BODY_PART = 1005,
    IDC_MODEL = 1006,
    IDC_STATS = 1007,
    IDC_CONFIDENCE = 1008,
    IDC_CONF_VALUE = 1009,
    IDC_AIM_METHOD = 1010,
    IDC_FPS_LIMIT = 1011,
    IDC_CAPTURE_METHOD = 1012,
    IDC_TAB_SETTINGS = 1013,
    IDC_TAB_VISUALS = 1014,
    IDC_VIS_REGION = 1015,
    IDC_VIS_BOXES = 1016,
    IDC_VIS_STATS = 1017,
    IDC_MOVEMENT_PATH = 1018,
    IDC_VIS_CONF = 1028,
    IDC_TAB_AI = 1019,
    IDC_SAVE_CONFIG = 1020,
    IDC_LOAD_CONFIG = 1021,
    IDC_TAB_CONFIG = 1022,
    IDC_CONFIG_LIST = 1023,
    IDC_CONFIG_NAME = 1024,
    IDC_DELETE_CONFIG = 1025,
    IDC_RENAME_CONFIG = 1026,
    IDC_AIM_BIND = 1029,
    IDC_WINDOW_MINIMIZE = 1030,
    IDC_WINDOW_MAXIMIZE = 1031,
    IDC_WINDOW_CLOSE = 1032,
    IDC_BODY_HEAD = 1033,
    IDC_BODY_NECK = 1034,
    IDC_BODY_TORSO = 1035,
    IDC_BODY_CLOSEST = 1036,
    IDC_HEAD_Y_OFFSET = 1037,
    IDC_HEAD_Y_VALUE = 1038,
    IDC_HEAD_Y_RESET = 1039,
    IDC_MOUSE_SCALE = 1040,
    IDC_MOUSE_SCALE_VALUE = 1041,
    IDC_CONFIG_NEW = 1043,
    IDC_CONFIG_CONFIRM = 1045,
    IDC_CONFIG_CANCEL = 1046,
};

