#include "../cogs_mikai.h"

// Custom events used by this scene
typedef enum {
    ResetSceneEventWriteNow = 1, // User chose "Write" in the confirm dialog
    ResetSceneEventClose = 2,    // User chose "Later" (or no-card error dismissed)
} ResetSceneEvent;

static void cogs_mikai_scene_reset_popup_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ResetSceneEventClose);
}

void cogs_mikai_scene_reset_on_enter(void* context) {
    COGSMyKaiApp* app = context;
    Popup* popup = app->popup;

    if(!app->mykey.is_loaded) {
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "No card loaded\nRead a card first", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_reset_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        return;
    }

    // Reset the card
    mykey_reset(&app->mykey);

    // Update cached values
    app->mykey.is_reset = mykey_is_reset(&app->mykey);
    app->mykey.current_credit = mykey_get_current_credit(&app->mykey);

    notification_message(app->notifications, &sequence_success);

    // Ask whether to write the reset state to the card right away
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Card Reset!", 64, 0, AlignCenter, AlignTop);
    dialog_message_set_text(message, "Write to card now?", 64, 28, AlignCenter, AlignTop);
    dialog_message_set_buttons(message, "Later", NULL, "Write");
    DialogMessageButton button = dialog_message_show(app->dialogs, message);
    dialog_message_free(message);

    // on_enter can't switch scenes directly, so route through an event
    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        button == DialogMessageButtonRight ? ResetSceneEventWriteNow : ResetSceneEventClose);
}

bool cogs_mikai_scene_reset_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == ResetSceneEventWriteNow) {
            // Write immediately: the write scene waits for the card
            scene_manager_next_scene(app->scene_manager, COGSMyKaiSceneWriteCard);
        } else {
            // Back to the start scene (menu is rebuilt, shows "Write to Card")
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, COGSMyKaiSceneStart);
        }
        consumed = true;
    }

    return consumed;
}

void cogs_mikai_scene_reset_on_exit(void* context) {
    COGSMyKaiApp* app = context;
    popup_reset(app->popup);
}
