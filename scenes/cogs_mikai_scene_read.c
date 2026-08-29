#include "../cogs_mikai.h"

// Custom events used by this scene.
// Values are large enough to never collide with submenu indexes of the
// start scene (queued events are dispatched to whatever scene is active).
typedef enum {
    ReadSceneEventResult = 0x100, // NFC read finished (app->read_result is valid)
    ReadSceneEventClose = 0x101,  // Result popup dismissed (timeout or key press)
} ReadSceneEvent;

// Worker thread: runs the NFC read off the GUI thread so the UI stays
// responsive (and so the user can cancel with Back while waiting).
static int32_t cogs_mikai_scene_read_worker(void* context) {
    COGSMyKaiApp* app = context;

    app->read_result = mykey_read_from_nfc(app);
    view_dispatcher_send_custom_event(app->view_dispatcher, ReadSceneEventResult);

    return 0;
}

static void cogs_mikai_scene_read_popup_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ReadSceneEventClose);
}

void cogs_mikai_scene_read_on_enter(void* context) {
    COGSMyKaiApp* app = context;

    // Make sure no stale worker is running (defensive: on_exit cleans it up)
    if(app->read_thread) {
        app->op_abort = true;
        furi_thread_join(app->read_thread);
        furi_thread_free(app->read_thread);
        app->read_thread = NULL;
    }

    // Reset read state
    app->op_abort = false;
    app->read_result = MyKeyReadResultNoCard;

    // Show popup while waiting for the card. No timeout and no callback:
    // the user cancels with Back, and the result is shown once the worker
    // reports back.
    Popup* popup = app->popup;
    popup_set_header(popup, "Reading Card", 64, 10, AlignCenter, AlignTop);
    popup_set_text(popup, "Place COGES MyKey\non Flipper's back", 64, 25, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 0, NULL);
    popup_set_context(popup, app);
    popup_disable_timeout(popup);

    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);

    // Start the NFC read on a dedicated thread
    app->read_thread = furi_thread_alloc_ex(
        "MyKeyReadWorker", 8 * 1024, cogs_mikai_scene_read_worker, app);
    furi_thread_start(app->read_thread);
}

bool cogs_mikai_scene_read_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == ReadSceneEventResult) {
            Popup* popup = app->popup;

            // Success confirmation is brief (1 s); errors stay on screen
            // longer so the user has time to read them.
            uint32_t result_timeout_ms = 2000;

            // Show the outcome of the read operation
            switch(app->read_result) {
            case MyKeyReadResultOk:
                result_timeout_ms = 1000;
                // Automatic backup: save a copy of the card to SD so it can
                // be restored later (Load from File). Failure is non-fatal.
                if(!mykey_backup_to_file(app)) {
                    FURI_LOG_W(TAG, "Automatic backup failed");
                }
                popup_set_header(popup, "Success!", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "Card read successfully", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_success);
                break;
            case MyKeyReadResultNoCard:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup,
                    "No card detected\nPlace card and retry",
                    64,
                    25,
                    AlignCenter,
                    AlignTop);
                notification_message(app->notifications, &sequence_error);
                break;
            case MyKeyReadResultUnsupportedCard:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup, "Card not supported\nSRIX4K required", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                break;
            case MyKeyReadResultReadFailed:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "Read failed\nTry again", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                break;
            case MyKeyReadResultNfcBusy:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup, "NFC is busy\nTry again", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                break;
            case MyKeyReadResultAborted:
                // Cancelled by the user: no message, just go back
                scene_manager_previous_scene(app->scene_manager);
                consumed = true;
                return consumed;
            }

            // Arm the popup timeout so the result closes by itself, and
            // re-enter the view to (re)start the timer. Any key press
            // also dismisses the popup.
            popup_set_callback(popup, cogs_mikai_scene_read_popup_callback);
            popup_set_context(popup, app);
            popup_set_timeout(popup, result_timeout_ms);
            popup_enable_timeout(popup);
            view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);

            consumed = true;
        } else if(event.event == ReadSceneEventClose) {
            if(app->read_result == MyKeyReadResultOk) {
                // Card loaded: jump straight to View Info so the user can
                // check the credit without going back through the menu
                scene_manager_next_scene(app->scene_manager, COGSMyKaiSceneInfo);
            } else {
                scene_manager_previous_scene(app->scene_manager);
            }
            consumed = true;
        }
    }

    return consumed;
}

void cogs_mikai_scene_read_on_exit(void* context) {
    COGSMyKaiApp* app = context;

    // Stop the reader thread if it is still running (user pressed Back
    // while the app was waiting for a card). The worker checks the abort
    // flag between detection attempts, so the join is quick.
    if(app->read_thread) {
        app->op_abort = true;
        furi_thread_join(app->read_thread);
        furi_thread_free(app->read_thread);
        app->read_thread = NULL;
    }

    popup_reset(app->popup);
}
