#include "../cogs_mikai.h"
#include <furi_hal_rtc.h>

// Custom events used by this scene
typedef enum {
    SetCreditEventAmount5 = 1,
    SetCreditEventAmount10,
    SetCreditEventAmount20,
    SetCreditEventAmount50,
    SetCreditEventCustom,     // "Custom amount..." selected
    SetCreditEventTextInput,  // Custom amount confirmed in text input
    SetCreditEventClose,      // Result popup dismissed (timeout or key press)
} SetCreditEvent;

// Predefined quick amounts shown as buttons in the submenu
typedef struct {
    const char* label;
    uint16_t cents;
} SetCreditPreset;

static const SetCreditPreset set_credit_presets[] = {
    {"5.00 EUR", 500},
    {"10.00 EUR", 1000},
    {"20.00 EUR", 2000},
    {"50.00 EUR", 5000},
};

#define SET_CREDIT_PRESETS_COUNT (sizeof(set_credit_presets) / sizeof(set_credit_presets[0]))

static bool cogs_mikai_scene_set_credit_validator(const char* text, FuriString* error, void* context) {
    UNUSED(context);

    if(strlen(text) == 0) {
        return true;
    }

    bool has_decimal = false;
    for(size_t i = 0; text[i] != '\0'; i++) {
        if(text[i] == '.' || text[i] == ',') {
            if(has_decimal) {
                furi_string_set(error, "Only one decimal point");
                return false;
            }
            has_decimal = true;
        } else if(text[i] < '0' || text[i] > '9') {
            furi_string_set(error, "Only numbers and '.'");
            return false;
        }
    }

    return true;
}

static bool parse_euros_to_cents(const char* text, uint16_t* cents) {
    if(!text || !cents) return false;

    uint32_t integer_part = 0;
    uint32_t decimal_part = 0;
    uint32_t decimal_digits = 0;
    bool in_decimal = false;

    for(size_t i = 0; text[i] != '\0'; i++) {
        if(text[i] == '.' || text[i] == ',') {
            in_decimal = true;
        } else if(text[i] >= '0' && text[i] <= '9') {
            if(in_decimal) {
                if(decimal_digits < 2) {
                    decimal_part = decimal_part * 10 + (text[i] - '0');
                    decimal_digits++;
                }
            } else {
                integer_part = integer_part * 10 + (text[i] - '0');
            }
        }
    }
    if(decimal_digits == 1) {
        decimal_part *= 10;
    }

    uint32_t total_cents = integer_part * 100 + decimal_part;
    if(total_cents > 99999) return false; // Max 999.99 EUR

    *cents = (uint16_t)total_cents;
    return true;
}

static void cogs_mikai_scene_set_credit_submenu_callback(void* context, uint32_t index) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void cogs_mikai_scene_set_credit_text_input_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SetCreditEventTextInput);
}

static void cogs_mikai_scene_set_credit_popup_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SetCreditEventClose);
}

// Set the card credit to the given amount, then jump to the write scene
static void cogs_mikai_scene_set_credit_apply(COGSMyKaiApp* app, uint16_t cents) {
    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    bool success = mykey_set_cents(
        &app->mykey, cents, datetime.day, datetime.month, datetime.year - 2000);

    if(!success) {
        Popup* popup = app->popup;
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "Failed to set credit", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_set_credit_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        notification_message(app->notifications, &sequence_error);
        return;
    }

    // cache updated credit
    app->mykey.current_credit = mykey_get_current_credit(&app->mykey);

    // clear text buffer so a stale value can't be re-applied
    memset(app->text_buffer, 0, sizeof(app->text_buffer));

    notification_message(app->notifications, &sequence_success);

    // Write straight away: the write scene waits for the card and writes.
    // Pressing Back there cancels the write (the credit stays in memory).
    scene_manager_next_scene(app->scene_manager, COGSMyKaiSceneWriteCard);
}

void cogs_mikai_scene_set_credit_on_enter(void* context) {
    COGSMyKaiApp* app = context;

    if(!app->mykey.is_loaded) {
        Popup* popup = app->popup;
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "No card loaded\nRead a card first", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_set_credit_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        return;
    }

    // Show predefined amount buttons + custom entry
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, "Set Credit (EUR)");

    for(size_t i = 0; i < SET_CREDIT_PRESETS_COUNT; i++) {
        submenu_add_item(
            submenu,
            set_credit_presets[i].label,
            SetCreditEventAmount5 + i,
            cogs_mikai_scene_set_credit_submenu_callback,
            app);
    }
    submenu_add_item(
        submenu,
        "Custom amount...",
        SetCreditEventCustom,
        cogs_mikai_scene_set_credit_submenu_callback,
        app);

    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewSubmenu);
}

bool cogs_mikai_scene_set_credit_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event >= SetCreditEventAmount5 && event.event < SetCreditEventCustom) {
            // Preset amount selected: set it directly
            uint16_t cents = set_credit_presets[event.event - SetCreditEventAmount5].cents;
            FURI_LOG_I(TAG, "Set credit preset: %u cents", cents);
            cogs_mikai_scene_set_credit_apply(app, cents);
            consumed = true;
        } else if(event.event == SetCreditEventCustom) {
            // Show text input for a custom amount
            TextInput* text_input = app->text_input;
            snprintf(app->text_buffer, sizeof(app->text_buffer), "10.00");

            text_input_set_header_text(text_input, "Set Credit (EUR)");
            text_input_set_validator(text_input, cogs_mikai_scene_set_credit_validator, NULL);
            text_input_set_result_callback(
                text_input,
                cogs_mikai_scene_set_credit_text_input_callback,
                app,
                app->text_buffer,
                sizeof(app->text_buffer),
                false);

            view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewTextInput);
            consumed = true;
        } else if(event.event == SetCreditEventTextInput) {
            FURI_LOG_I(TAG, "Set credit input: '%s'", app->text_buffer);
            uint16_t cents = 0;
            bool parse_ok = parse_euros_to_cents(app->text_buffer, &cents);
            FURI_LOG_I(TAG, "Parse result: %d, cents: %d", parse_ok, cents);

            if(parse_ok) {
                cogs_mikai_scene_set_credit_apply(app, cents);
            } else {
                FURI_LOG_E(TAG, "Invalid amount: parse_ok=%d, cents=%d", parse_ok, cents);
                Popup* popup = app->popup;
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup,
                    "Invalid amount\nEnter 0.00-999.99",
                    64,
                    25,
                    AlignCenter,
                    AlignTop);
                popup_set_callback(popup, cogs_mikai_scene_set_credit_popup_callback);
                popup_set_context(popup, app);
                popup_set_timeout(popup, 2000);
                popup_enable_timeout(popup);
                view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
                notification_message(app->notifications, &sequence_error);
            }
            consumed = true;
        } else if(event.event == SetCreditEventClose) {
            // Result popup dismissed: back to the amount list to retry
            view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewSubmenu);
            consumed = true;
        }
    }

    return consumed;
}

void cogs_mikai_scene_set_credit_on_exit(void* context) {
    COGSMyKaiApp* app = context;
    submenu_reset(app->submenu);
    text_input_reset(app->text_input);
    popup_reset(app->popup);
}
