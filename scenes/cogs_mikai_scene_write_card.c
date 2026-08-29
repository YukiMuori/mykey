#include "../cogs_mikai.h"

// Custom events used by this scene (large values: no collision with the
// start scene submenu indexes).
typedef enum {
    WriteCardEventResult = 0x100, // NFC write finished (app->write_result valid)
    WriteCardEventClose = 0x101,  // Result popup dismissed (timeout or key press)
} WriteCardEvent;

// Worker thread: runs the NFC write off the GUI thread so the UI stays
// responsive (and so the user can cancel with Back while waiting).
static int32_t cogs_mikai_scene_write_card_worker(void* context) {
    COGSMyKaiApp* app = context;

    app->write_result = mykey_write_to_nfc(app);
    view_dispatcher_send_custom_event(app->view_dispatcher, WriteCardEventResult);

    return 0;
}

static void cogs_mikai_scene_write_card_popup_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WriteCardEventClose);
}

void cogs_mikai_scene_write_card_on_enter(void* context) {
    COGSMyKaiApp* app = context;
    Popup* popup = app->popup;

    // Make sure no stale worker is running (defensive: on_exit cleans it up)
    if(app->write_thread) {
        app->op_abort = true;
        furi_thread_join(app->write_thread);
        furi_thread_free(app->write_thread);
        app->write_thread = NULL;
    }

    if(!app->mykey.is_loaded) {
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "No card loaded", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_write_card_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        return;
    }

    if(!app->mykey.is_modified) {
        popup_set_header(popup, "No Changes", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "Card data not modified", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_write_card_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        return;
    }

    // Reset write state
    app->op_abort = false;
    app->write_result = false;

    // Show popup while waiting for the card. No timeout: the user cancels
    // with Back, and the result is shown once the worker reports back.
    popup_set_header(popup, "Writing...", 64, 10, AlignCenter, AlignTop);
    popup_set_text(popup, "Place card on reader", 64, 25, AlignCenter, AlignTop);
    popup_set_context(popup, app);
    popup_disable_timeout(popup);

    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);

    // Start the NFC write on a dedicated thread
    app->write_thread = furi_thread_alloc_ex(
        "MyKeyWriteWorker", 8 * 1024, cogs_mikai_scene_write_card_worker, app);
    furi_thread_start(app->write_thread);
}

bool cogs_mikai_scene_write_card_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WriteCardEventResult) {
            Popup* popup = app->popup;

            // Show the outcome of the write operation
            if(app->write_result) {
                app->mykey.is_modified = false;
                popup_set_header(popup, "Success!", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "Card updated", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_success);
                popup_set_timeout(popup, 1000);
            } else {
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "Write failed\nTry again", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                popup_set_timeout(popup, 2000);
            }

            popup_set_callback(popup, cogs_mikai_scene_write_card_popup_callback);
            popup_set_context(popup, app);
            popup_enable_timeout(popup);
            view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);

            consumed = true;
        } else if(event.event == WriteCardEventClose) {
            // Back to the start scene (menu is rebuilt: "Write to Card"
            // disappears after a successful write)
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, COGSMyKaiSceneStart);
            consumed = true;
        }
    }

    return consumed;
}

void cogs_mikai_scene_write_card_on_exit(void* context) {
    COGSMyKaiApp* app = context;

    // Stop the writer thread if it is still running (user pressed Back
    // while waiting for the card)
    if(app->write_thread) {
        app->op_abort = true;
        furi_thread_join(app->write_thread);
        furi_thread_free(app->write_thread);
        app->write_thread = NULL;
    }

    popup_reset(app->popup);
}
