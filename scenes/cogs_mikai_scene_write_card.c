#include "../cogs_mikai.h"

// Custom events used by this scene (large values: no collision with the
// start scene submenu indexes).
typedef enum {
    WriteCardEventResult = 0x100, // NFC write finished (app->write_result valid)
    WriteCardEventClose = 0x101,  // Result popup dismissed (timeout or key press)
    WriteCardEventConfirmWrite = 0x102, // User confirmed writing to a different card
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

// (Re)start the write worker
static void cogs_mikai_scene_write_card_start_worker(COGSMyKaiApp* app) {
    app->op_abort = false;
    app->write_result = MyKeyWriteResultNoCard;
    app->write_verify_total = 0;
    app->write_verify_ok = 0;

    app->write_thread = furi_thread_alloc_ex(
        "MyKeyWriteWorker", 8 * 1024, cogs_mikai_scene_write_card_worker, app);
    furi_thread_start(app->write_thread);
}

// Stop and free the write worker if it is still running
static void cogs_mikai_scene_write_card_stop_worker(COGSMyKaiApp* app) {
    if(app->write_thread) {
        app->op_abort = true;
        furi_thread_join(app->write_thread);
        furi_thread_free(app->write_thread);
        app->write_thread = NULL;
    }
}

// Show the "place card on reader" popup (used when starting/restarting)
static void cogs_mikai_scene_write_card_show_waiting(COGSMyKaiApp* app) {
    Popup* popup = app->popup;
    popup_set_header(popup, "Writing...", 64, 10, AlignCenter, AlignTop);
    popup_set_text(popup, "Place card on reader", 64, 25, AlignCenter, AlignTop);
    popup_set_context(popup, app);
    popup_disable_timeout(popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
}

void cogs_mikai_scene_write_card_on_enter(void* context) {
    COGSMyKaiApp* app = context;
    Popup* popup = app->popup;

    // Make sure no stale worker is running (defensive: on_exit cleans it up)
    cogs_mikai_scene_write_card_stop_worker(app);

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

    // Fresh write: UID check is enabled
    app->write_force = false;

    cogs_mikai_scene_write_card_show_waiting(app);
    cogs_mikai_scene_write_card_start_worker(app);
}

bool cogs_mikai_scene_write_card_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WriteCardEventResult) {
            Popup* popup = app->popup;

            switch(app->write_result) {
            case MyKeyWriteResultOk:
                app->mykey.is_modified = false;
                popup_set_header(popup, "Success!", 64, 10, AlignCenter, AlignTop);
                // Show the resulting balance (and the verification result
                // when the read-back check ran)
                snprintf(
                    app->text_buffer,
                    sizeof(app->text_buffer),
                    "Card updated\nCredit: %u.%02u EUR",
                    app->mykey.current_credit / 100,
                    app->mykey.current_credit % 100);
                if(app->write_verify_total > 0) {
                    snprintf(
                        app->text_buffer + strlen(app->text_buffer),
                        sizeof(app->text_buffer) - strlen(app->text_buffer),
                        "\nVerify: %u/%u blocks OK",
                        app->write_verify_ok,
                        app->write_verify_total);
                }
                popup_set_text(popup, app->text_buffer, 64, 20, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_success);
                popup_set_timeout(popup, 1000);
                break;

            case MyKeyWriteResultVerifyFailed:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                snprintf(
                    app->text_buffer,
                    sizeof(app->text_buffer),
                    "Verify failed\n%u/%u blocks OK\nRetry write",
                    app->write_verify_ok,
                    app->write_verify_total);
                popup_set_text(popup, app->text_buffer, 64, 20, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                popup_set_timeout(popup, 2000);
                break;

            case MyKeyWriteResultUidMismatch: {
                // The card on the reader is not the one that was read.
                // Ask for an explicit confirmation before overwriting it.
                DialogMessage* message = dialog_message_alloc();
                dialog_message_set_header(message, "Card mismatch", 64, 0, AlignCenter, AlignTop);
                dialog_message_set_text(
                    message,
                    "Card on reader differs\nfrom loaded one.\nWrite anyway?",
                    64,
                    28,
                    AlignCenter,
                    AlignTop);
                dialog_message_set_buttons(message, "No", NULL, "Yes");
                DialogMessageButton button = dialog_message_show(app->dialogs, message);
                dialog_message_free(message);

                if(button == DialogMessageButtonRight) {
                    // Confirmed: restart the write skipping the UID check
                    app->write_force = true;
                    cogs_mikai_scene_write_card_show_waiting(app);
                    cogs_mikai_scene_write_card_start_worker(app);
                } else {
                    // Refused: back to the menu, nothing was written
                    scene_manager_search_and_switch_to_previous_scene(
                        app->scene_manager, COGSMyKaiSceneStart);
                }
                consumed = true;
                return consumed;
            }

            case MyKeyWriteResultNoCard:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup, "No card detected\nPlace card and retry", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                popup_set_timeout(popup, 2000);
                break;

            case MyKeyWriteResultUnsupportedCard:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup, "Card not supported\nSRIX4K required", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                popup_set_timeout(popup, 2000);
                break;

            case MyKeyWriteResultWriteFailed:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "Write failed\nTry again", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                popup_set_timeout(popup, 2000);
                break;

            case MyKeyWriteResultNfcBusy:
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "NFC is busy\nTry again", 64, 25, AlignCenter, AlignTop);
                notification_message(app->notifications, &sequence_error);
                popup_set_timeout(popup, 2000);
                break;

            case MyKeyWriteResultAborted:
                // Cancelled by the user: no message, just go back
                scene_manager_search_and_switch_to_previous_scene(
                    app->scene_manager, COGSMyKaiSceneStart);
                consumed = true;
                return consumed;
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
        } else if(event.event == WriteCardEventConfirmWrite) {
            // (reserved, currently unused)
            consumed = true;
        }
    }

    return consumed;
}

void cogs_mikai_scene_write_card_on_exit(void* context) {
    COGSMyKaiApp* app = context;

    // Stop the writer thread if it is still running (user pressed Back
    // while waiting for the card)
    cogs_mikai_scene_write_card_stop_worker(app);

    popup_reset(app->popup);
}
